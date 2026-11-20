// android.media.AudioSystem: the framework's window onto the audio server (AudioFlinger
// and the audio policy service). There is no audio server here; sound is played through
// AVFoundation by the classes that produce it (see media_jni.mm). What remains are the
// few facts and identifiers the Java audio classes ask for on their way there.

#include <atomic>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

// Limits of the audio framework's own data types, which AudioSystem reads when it loads.
jint AudioSystem_getMaxChannelCount(JNIEnv*, jclass) {
    return 24;
}
jint AudioSystem_getMaxSampleRate(JNIEnv*, jclass) {
    return 192000;
}
jint AudioSystem_getMinSampleRate(JNIEnv*, jclass) {
    return 4000;
}

// Audio sessions group a player with the effects applied to it. Nothing attaches effects
// here, but each player still gets its own id.
jint AudioSystem_newAudioSessionId(JNIEnv*, jclass) {
    static std::atomic<jint> next{1};
    return next++;
}

// The audio inputs and outputs ("ports") and their connections. None are listed: sound
// goes to wherever iOS routes it, which is not described to Android code yet.
jint AudioSystem_listAudioPorts(JNIEnv*, jclass, jobject, jintArray) {
    return 0;  // AudioSystem.SUCCESS, with the list left empty
}
jint AudioSystem_listAudioPatches(JNIEnv*, jclass, jobject, jintArray) {
    return 0;
}

// android.media.AudioPortEventHandler: notifications about ports changing.
void AudioPortEventHandler_setup(JNIEnv*, jobject, jobject) {}
void AudioPortEventHandler_finalize(JNIEnv*, jobject) {}

const JNINativeMethod kPortEventMethods[] = {
        {"native_setup", "(Ljava/lang/Object;)V", (void*)AudioPortEventHandler_setup},
        {"native_finalize", "()V", (void*)AudioPortEventHandler_finalize},
};

const JNINativeMethod kMethods[] = {
        {"listAudioPorts", "(Ljava/util/ArrayList;[I)I", (void*)AudioSystem_listAudioPorts},
        {"listAudioPatches", "(Ljava/util/ArrayList;[I)I", (void*)AudioSystem_listAudioPatches},
        {"native_getMaxChannelCount", "()I", (void*)AudioSystem_getMaxChannelCount},
        {"native_getMaxSampleRate", "()I", (void*)AudioSystem_getMaxSampleRate},
        {"native_getMinSampleRate", "()I", (void*)AudioSystem_getMinSampleRate},
        {"newAudioSessionId", "()I", (void*)AudioSystem_newAudioSessionId},
};

}  // namespace

int register_android_media_AudioSystem(JNIEnv* env) {
    if (jniRegisterNativeMethods(env, "android/media/AudioPortEventHandler", kPortEventMethods,
                                 NELEM(kPortEventMethods)) < 0) {
        return -1;
    }
    return jniRegisterNativeMethods(env, "android/media/AudioSystem", kMethods, NELEM(kMethods));
}

}  // namespace android
