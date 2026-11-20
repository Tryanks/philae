// libmedia_jni: the native half of android.media.MediaPlayer, on AVFoundation.
//
// On a device a MediaPlayer is a client of the media server, which decodes and plays in
// another process. Here each MediaPlayer owns an AVAudioPlayer. That covers what apps use
// MediaPlayer for most: playing a sound from the APK or from a file, with the usual
// start/pause/seek/loop/volume controls and the prepared/completed/error callbacks.
//
// Not covered yet: video (there is no Surface to render to), network streams, and
// formats AVFoundation cannot decode (Ogg Vorbis, notably). Those fail in prepare() with
// the IOException an app would get for an unsupported file on a device.

#import <AVFoundation/AVFoundation.h>

#include <mutex>
#include <unistd.h>

#include <nativehelper/JNIHelp.h>

namespace {

// MediaPlayer's event codes (the MEDIA_* constants in MediaPlayer.java).
enum {
    kMediaPrepared = 1,
    kMediaPlaybackComplete = 2,
    kMediaSeekComplete = 4,
    kMediaError = 100,
};
const int kMediaErrorUnknown = 1;       // MediaPlayer.MEDIA_ERROR_UNKNOWN
const int kAudioStreamMusic = 3;        // AudioManager.STREAM_MUSIC

JavaVM* gVM;
jclass gMediaPlayerClass;
jfieldID gNativeContextField;
jmethodID gPostEventMethod;
jmethodID gFileDescriptorGetInt;

JNIEnv* CurrentEnv() {
    JNIEnv* env = nullptr;
    if (gVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return gVM->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

}  // namespace

// One MediaPlayer's native state. All methods may be called from any thread.
@interface A2IMediaPlayer : NSObject <AVAudioPlayerDelegate>
@property(nonatomic) jobject weakThis;  // global reference to the WeakReference<MediaPlayer>
@property(nonatomic) int audioSessionId;
- (void)setData:(NSData*)data;
- (BOOL)prepare;
- (void)postEvent:(int)what arg1:(int)arg1 arg2:(int)arg2;
@end

@implementation A2IMediaPlayer {
    std::mutex _lock;
    NSData* _data;
    AVAudioPlayer* _player;
    BOOL _looping;
    float _volume;
    float _pan;
}

- (instancetype)init {
    self = [super init];
    if (self) _volume = 1;
    return self;
}

- (void)setData:(NSData*)data {
    std::lock_guard<std::mutex> guard(_lock);
    _data = data;
    _player = nil;
}

- (BOOL)hasData {
    std::lock_guard<std::mutex> guard(_lock);
    return _data != nil;
}

- (BOOL)prepare {
    std::lock_guard<std::mutex> guard(_lock);
    if (_player != nil) return YES;
    if (_data == nil) return NO;
    NSError* error = nil;
    AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:_data error:&error];
    if (player == nil || ![player prepareToPlay]) {
        NSLog(@"MediaPlayer: cannot play this data: %@", error);
        return NO;
    }
    player.delegate = self;
    player.numberOfLoops = _looping ? -1 : 0;
    player.volume = _volume;
    player.pan = _pan;
    _player = player;
    return YES;
}

- (BOOL)start {
    std::lock_guard<std::mutex> guard(_lock);
    if (_player == nil) return NO;
    // Android plays media regardless of the ringer switch and alongside other apps'
    // audio unless an app asks for audio focus.
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        AVAudioSession* session = AVAudioSession.sharedInstance;
        [session setCategory:AVAudioSessionCategoryPlayback
                 withOptions:AVAudioSessionCategoryOptionMixWithOthers
                       error:nil];
        [session setActive:YES error:nil];
    });
    return [_player play];
}

- (void)pause {
    std::lock_guard<std::mutex> guard(_lock);
    [_player pause];
}

- (void)stop {
    std::lock_guard<std::mutex> guard(_lock);
    [_player stop];
    _player = nil;  // as on Android, a stopped player must be prepared again
}

- (void)reset {
    std::lock_guard<std::mutex> guard(_lock);
    [_player stop];
    _player.delegate = nil;
    _player = nil;
    _data = nil;
    _looping = NO;
    _volume = 1;
    _pan = 0;
}

- (BOOL)isPlaying {
    std::lock_guard<std::mutex> guard(_lock);
    return _player.playing;
}

- (int)positionMs {
    std::lock_guard<std::mutex> guard(_lock);
    return (int)lround(_player.currentTime * 1000);
}

- (int)durationMs {
    std::lock_guard<std::mutex> guard(_lock);
    return _player != nil ? (int)lround(_player.duration * 1000) : -1;
}

- (void)seekToMs:(long long)ms {
    std::lock_guard<std::mutex> guard(_lock);
    _player.currentTime = MIN(MAX(ms / 1000.0, 0), _player.duration);
}

- (void)setLooping:(BOOL)looping {
    std::lock_guard<std::mutex> guard(_lock);
    _looping = looping;
    _player.numberOfLoops = looping ? -1 : 0;
}

- (BOOL)looping {
    std::lock_guard<std::mutex> guard(_lock);
    return _looping;
}

- (void)setLeftVolume:(float)left rightVolume:(float)right {
    std::lock_guard<std::mutex> guard(_lock);
    _volume = MAX(left, right);
    _pan = _volume > 0 ? (right - left) / _volume : 0;
    _player.volume = _volume;
    _player.pan = _pan;
}

// Calls MediaPlayer.postEventFromNative, which delivers the event to the app's listeners
// on the thread the MediaPlayer was created on.
- (void)postEvent:(int)what arg1:(int)arg1 arg2:(int)arg2 {
    JNIEnv* env = CurrentEnv();
    if (env == nullptr || _weakThis == nullptr) return;
    env->CallStaticVoidMethod(gMediaPlayerClass, gPostEventMethod, _weakThis, what, arg1, arg2, nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)successfully {
    if (successfully) {
        [self postEvent:kMediaPlaybackComplete arg1:0 arg2:0];
    } else {
        [self postEvent:kMediaError arg1:kMediaErrorUnknown arg2:0];
    }
}

- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)player error:(NSError*)error {
    NSLog(@"MediaPlayer: decode error: %@", error);
    [self postEvent:kMediaError arg1:kMediaErrorUnknown arg2:0];
}

@end

namespace {

// The native player of a Java MediaPlayer, or nil (with IllegalStateException pending)
// if it has been released.
A2IMediaPlayer* PlayerOf(JNIEnv* env, jobject thiz) {
    void* context = reinterpret_cast<void*>(env->GetLongField(thiz, gNativeContextField));
    if (context == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "the MediaPlayer has been released");
        return nil;
    }
    return (__bridge A2IMediaPlayer*)context;
}

void ReleasePlayer(JNIEnv* env, jobject thiz) {
    void* context = reinterpret_cast<void*>(env->GetLongField(thiz, gNativeContextField));
    if (context == nullptr) return;
    env->SetLongField(thiz, gNativeContextField, 0);
    A2IMediaPlayer* player = (__bridge_transfer A2IMediaPlayer*)context;
    [player reset];
    jobject weakThis = player.weakThis;
    player.weakThis = nullptr;
    if (weakThis != nullptr) env->DeleteGlobalRef(weakThis);
}

void MediaPlayer_native_init(JNIEnv*, jclass) {}

void MediaPlayer_native_setup(JNIEnv* env, jobject thiz, jobject weakThis, jobject, jint audioSessionId) {
    A2IMediaPlayer* player = [A2IMediaPlayer new];
    player.weakThis = env->NewGlobalRef(weakThis);
    player.audioSessionId = audioSessionId;
    env->SetLongField(thiz, gNativeContextField, reinterpret_cast<jlong>((__bridge_retained void*)player));
}

void MediaPlayer_release(JNIEnv* env, jobject thiz) {
    ReleasePlayer(env, thiz);
}

void MediaPlayer_reset(JNIEnv* env, jobject thiz) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    [player reset];
}

// A file, or part of one (a sound stored in the APK is a byte range of the APK).
void MediaPlayer_setDataSourceFd(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset, jlong length) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player == nil) return;
    if (fileDescriptor == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    int fd = env->CallIntMethod(fileDescriptor, gFileDescriptorGetInt);
    NSMutableData* data = [NSMutableData data];
    char buffer[64 * 1024];
    off_t position = offset;
    jlong remaining = length;
    while (remaining > 0) {
        ssize_t n = pread(fd, buffer, (size_t)MIN((jlong)sizeof buffer, remaining), position);
        if (n < 0) {
            jniThrowException(env, "java/io/IOException", "setDataSourceFD failed: cannot read the file");
            return;
        }
        if (n == 0) break;  // "to the end of the file" is passed as a huge length
        [data appendBytes:buffer length:(NSUInteger)n];
        position += n;
        remaining -= n;
    }
    [player setData:data];
}

// A path or URL.
void MediaPlayer_setDataSourcePath(JNIEnv* env, jobject thiz, jobject, jstring path, jobjectArray, jobjectArray) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player == nil) return;
    if (path == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const char* chars = env->GetStringUTFChars(path, nullptr);
    NSString* location = @(chars);
    env->ReleaseStringUTFChars(path, chars);
    if ([location hasPrefix:@"file://"]) location = [NSURL URLWithString:location].path;
    NSData* data = [location hasPrefix:@"/"] ? [NSData dataWithContentsOfFile:location] : nil;
    if (data == nil) {
        jniThrowException(env, "java/io/IOException",
                          [NSString stringWithFormat:@"setDataSource failed: cannot open %@", location].UTF8String);
        return;
    }
    [player setData:data];
}

void MediaPlayer_setDataSourceCallback(JNIEnv* env, jobject, jobject) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "MediaDataSource is not supported");
}

jint MediaPlayer_prepare(JNIEnv* env, jobject thiz, jobject) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player == nil) return 0;
    if (![player hasData]) {
        jniThrowException(env, "java/lang/IllegalStateException", "prepare() called without a data source");
        return 0;
    }
    if (![player prepare]) {
        jniThrowException(env, "java/io/IOException", "Prepare failed.: status=0x1");
        return 0;
    }
    [player postEvent:kMediaPrepared arg1:0 arg2:0];
    return 0;
}

jint MediaPlayer_prepareAsync(JNIEnv* env, jobject thiz, jobject) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player == nil) return 0;
    if (![player hasData]) {
        jniThrowException(env, "java/lang/IllegalStateException", "prepareAsync() called without a data source");
        return 0;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if ([player prepare]) {
            [player postEvent:kMediaPrepared arg1:0 arg2:0];
        } else {
            [player postEvent:kMediaError arg1:kMediaErrorUnknown arg2:0];
        }
    });
    return 0;
}

void MediaPlayer_start(JNIEnv* env, jobject thiz) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player != nil && ![player start]) {
        jniThrowException(env, "java/lang/IllegalStateException", "start() called on a player that is not prepared");
    }
}

void MediaPlayer_stop(JNIEnv* env, jobject thiz) {
    [PlayerOf(env, thiz) stop];
}

void MediaPlayer_pause(JNIEnv* env, jobject thiz) {
    [PlayerOf(env, thiz) pause];
}

jboolean MediaPlayer_isPlaying(JNIEnv* env, jobject thiz) {
    return [PlayerOf(env, thiz) isPlaying];
}

void MediaPlayer_seekTo(JNIEnv* env, jobject thiz, jlong ms, jint) {
    A2IMediaPlayer* player = PlayerOf(env, thiz);
    if (player == nil) return;
    [player seekToMs:ms];
    [player postEvent:kMediaSeekComplete arg1:0 arg2:0];
}

jint MediaPlayer_getCurrentPosition(JNIEnv* env, jobject thiz) {
    return [PlayerOf(env, thiz) positionMs];
}

jint MediaPlayer_getDuration(JNIEnv* env, jobject thiz) {
    return [PlayerOf(env, thiz) durationMs];
}

void MediaPlayer_setLooping(JNIEnv* env, jobject thiz, jboolean looping) {
    [PlayerOf(env, thiz) setLooping:looping];
}

jboolean MediaPlayer_isLooping(JNIEnv* env, jobject thiz) {
    return [PlayerOf(env, thiz) looping];
}

void MediaPlayer_setVolume(JNIEnv* env, jobject thiz, jfloat left, jfloat right) {
    [PlayerOf(env, thiz) setLeftVolume:left rightVolume:right];
}

void MediaPlayer_setAudioSessionId(JNIEnv* env, jobject thiz, jint id) {
    PlayerOf(env, thiz).audioSessionId = id;
}

jint MediaPlayer_getAudioSessionId(JNIEnv* env, jobject thiz) {
    return PlayerOf(env, thiz).audioSessionId;
}

void MediaPlayer_finalize(JNIEnv* env, jobject thiz) {
    ReleasePlayer(env, thiz);
}

// The rest of the interface: accepted, with nothing to do for an audio-only player that
// has no routing, effects, metadata or DRM.
void MediaPlayer_setVideoSurface(JNIEnv*, jobject, jobject) {}
jint MediaPlayer_getAudioStreamType(JNIEnv*, jobject) { return kAudioStreamMusic; }
void MediaPlayer_setAudioStreamType(JNIEnv*, jobject, jint) {}
jint MediaPlayer_applyVolumeShaper(JNIEnv*, jobject, jobject, jobject) { return 0; }
jobject MediaPlayer_getVolumeShaperState(JNIEnv*, jobject, jint) { return nullptr; }
jboolean MediaPlayer_setOutputDevice(JNIEnv*, jobject, jint) { return JNI_FALSE; }
jint MediaPlayer_getRoutedDeviceId(JNIEnv*, jobject) { return 0; }
void MediaPlayer_enableDeviceCallback(JNIEnv*, jobject, jboolean) {}
jint MediaPlayer_getVideoSize(JNIEnv*, jobject) { return 0; }
jobject MediaPlayer_getMetrics(JNIEnv*, jobject) { return nullptr; }
void MediaPlayer_setNextMediaPlayer(JNIEnv*, jobject, jobject) {}
void MediaPlayer_notifyAt(JNIEnv*, jobject, jlong) {}
jboolean MediaPlayer_setParameter(JNIEnv*, jobject, jint, jobject) { return JNI_TRUE; }
void MediaPlayer_attachAuxEffect(JNIEnv*, jobject, jint) {}
void MediaPlayer_setAuxEffectSendLevel(JNIEnv*, jobject, jfloat) {}
// Requests through the generic invoke() channel (track info and selection): success,
// with an empty reply, which reads as "no tracks".
jint MediaPlayer_invoke(JNIEnv*, jobject, jobject, jobject) { return 0; }
jboolean MediaPlayer_getMetadata(JNIEnv*, jobject, jboolean, jboolean, jobject) { return JNI_FALSE; }
jint MediaPlayer_setMetadataFilter(JNIEnv*, jobject, jobject) { return 0; }
jint MediaPlayer_pullBatteryData(JNIEnv*, jclass, jobject) { return 0; }
jint MediaPlayer_setRetransmitEndpoint(JNIEnv*, jobject, jstring, jint) { return 0; }

const JNINativeMethod kMediaPlayerMethods[] = {
        {"native_init", "()V", (void*)MediaPlayer_native_init},
        {"native_setup", "(Ljava/lang/Object;Landroid/os/Parcel;I)V", (void*)MediaPlayer_native_setup},
        {"native_finalize", "()V", (void*)MediaPlayer_finalize},
        {"_release", "()V", (void*)MediaPlayer_release},
        {"_reset", "()V", (void*)MediaPlayer_reset},
        {"_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", (void*)MediaPlayer_setDataSourceFd},
        {"nativeSetDataSource",
         "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
         (void*)MediaPlayer_setDataSourcePath},
        {"_setDataSource", "(Landroid/media/MediaDataSource;)V", (void*)MediaPlayer_setDataSourceCallback},
        {"_prepare", "(Landroid/os/Parcel;)I", (void*)MediaPlayer_prepare},
        {"_prepareAsync", "(Landroid/os/Parcel;)I", (void*)MediaPlayer_prepareAsync},
        {"_start", "()V", (void*)MediaPlayer_start},
        {"_stop", "()V", (void*)MediaPlayer_stop},
        {"_pause", "()V", (void*)MediaPlayer_pause},
        {"isPlaying", "()Z", (void*)MediaPlayer_isPlaying},
        {"_seekTo", "(JI)V", (void*)MediaPlayer_seekTo},
        {"getCurrentPosition", "()I", (void*)MediaPlayer_getCurrentPosition},
        {"getDuration", "()I", (void*)MediaPlayer_getDuration},
        {"setLooping", "(Z)V", (void*)MediaPlayer_setLooping},
        {"isLooping", "()Z", (void*)MediaPlayer_isLooping},
        {"_setVolume", "(FF)V", (void*)MediaPlayer_setVolume},
        {"native_setAudioSessionId", "(I)V", (void*)MediaPlayer_setAudioSessionId},
        {"getAudioSessionId", "()I", (void*)MediaPlayer_getAudioSessionId},
        {"_setVideoSurface", "(Landroid/view/Surface;)V", (void*)MediaPlayer_setVideoSurface},
        {"_getAudioStreamType", "()I", (void*)MediaPlayer_getAudioStreamType},
        {"_setAudioStreamType", "(I)V", (void*)MediaPlayer_setAudioStreamType},
        {"native_applyVolumeShaper",
         "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I",
         (void*)MediaPlayer_applyVolumeShaper},
        {"native_getVolumeShaperState", "(I)Landroid/media/VolumeShaper$State;",
         (void*)MediaPlayer_getVolumeShaperState},
        {"native_setOutputDevice", "(I)Z", (void*)MediaPlayer_setOutputDevice},
        {"native_getRoutedDeviceId", "()I", (void*)MediaPlayer_getRoutedDeviceId},
        {"native_enableDeviceCallback", "(Z)V", (void*)MediaPlayer_enableDeviceCallback},
        {"getVideoWidth", "()I", (void*)MediaPlayer_getVideoSize},
        {"getVideoHeight", "()I", (void*)MediaPlayer_getVideoSize},
        {"native_getMetrics", "()Landroid/os/PersistableBundle;", (void*)MediaPlayer_getMetrics},
        {"setNextMediaPlayer", "(Landroid/media/MediaPlayer;)V", (void*)MediaPlayer_setNextMediaPlayer},
        {"_notifyAt", "(J)V", (void*)MediaPlayer_notifyAt},
        {"setParameter", "(ILandroid/os/Parcel;)Z", (void*)MediaPlayer_setParameter},
        {"attachAuxEffect", "(I)V", (void*)MediaPlayer_attachAuxEffect},
        {"_setAuxEffectSendLevel", "(F)V", (void*)MediaPlayer_setAuxEffectSendLevel},
        {"native_invoke", "(Landroid/os/Parcel;Landroid/os/Parcel;)I", (void*)MediaPlayer_invoke},
        {"native_getMetadata", "(ZZLandroid/os/Parcel;)Z", (void*)MediaPlayer_getMetadata},
        {"native_setMetadataFilter", "(Landroid/os/Parcel;)I", (void*)MediaPlayer_setMetadataFilter},
        {"native_pullBatteryData", "(Landroid/os/Parcel;)I", (void*)MediaPlayer_pullBatteryData},
        {"native_setRetransmitEndpoint", "(Ljava/lang/String;I)I", (void*)MediaPlayer_setRetransmitEndpoint},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    gVM = vm;
    jclass mediaPlayer = env->FindClass("android/media/MediaPlayer");
    jclass fileDescriptor = env->FindClass("java/io/FileDescriptor");
    if (mediaPlayer == nullptr || fileDescriptor == nullptr) return JNI_ERR;
    gMediaPlayerClass = static_cast<jclass>(env->NewGlobalRef(mediaPlayer));
    gNativeContextField = env->GetFieldID(mediaPlayer, "mNativeContext", "J");
    gPostEventMethod = env->GetStaticMethodID(mediaPlayer, "postEventFromNative",
                                              "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    gFileDescriptorGetInt = env->GetMethodID(fileDescriptor, "getInt$", "()I");
    if (gNativeContextField == nullptr || gPostEventMethod == nullptr || gFileDescriptorGetInt == nullptr) {
        return JNI_ERR;
    }
    if (jniRegisterNativeMethods(env, "android/media/MediaPlayer", kMediaPlayerMethods,
                                 NELEM(kMediaPlayerMethods)) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
