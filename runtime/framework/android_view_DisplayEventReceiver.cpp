// android.view.DisplayEventReceiver: the source of vsync for Choreographer.
//
// On a device these events come from SurfaceFlinger over a socket watched by the
// receiver's Looper. Here a single timer thread plays the display: a receiver asks for
// the next vsync (nativeScheduleVsync) and gets one dispatchVsync() call at the next
// 60 Hz tick. Choreographer's receiver does not act on the calling thread; it posts the
// frame to its own handler, so delivering from the timer thread is sufficient.
//
// TODO: on iOS proper the ticks should come from CADisplayLink so that frames are aligned
// with the real display refresh.

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <vector>

#include <android_runtime/AndroidRuntime.h>
#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

constexpr jlong kFrameIntervalNanos = 16666667;

struct Receiver {
    jobject receiver_weak;         // global ref to WeakReference<DisplayEventReceiver>
    jobject vsync_data_weak;       // global ref to WeakReference<VsyncEventData>
    bool vsync_requested = false;
};

std::mutex gLock;
std::vector<Receiver*>& Receivers() {
    static auto* receivers = new std::vector<Receiver*>();
    return *receivers;
}
pthread_once_t gThreadOnce = PTHREAD_ONCE_INIT;

struct {
    jmethodID weak_get;            // java.lang.ref.Reference.get()
    jmethodID dispatch_vsync;      // DisplayEventReceiver.dispatchVsync(JJI)V
    jmethodID nano_time;           // System.nanoTime()
    jclass system_class;
    jfieldID frame_interval, preferred_index, timelines_length, timelines;
    jfieldID timeline_vsync_id, timeline_expected_presentation, timeline_deadline;
} gIds;

void FillVsyncData(JNIEnv* env, jobject data, jlong timestamp, jlong frame) {
    env->SetLongField(data, gIds.frame_interval, kFrameIntervalNanos);
    env->SetIntField(data, gIds.preferred_index, 0);
    env->SetIntField(data, gIds.timelines_length, 1);
    jobjectArray timelines = static_cast<jobjectArray>(env->GetObjectField(data, gIds.timelines));
    jobject timeline = timelines != nullptr ? env->GetObjectArrayElement(timelines, 0) : nullptr;
    if (timeline != nullptr) {
        env->SetLongField(timeline, gIds.timeline_vsync_id, frame);
        env->SetLongField(timeline, gIds.timeline_expected_presentation, timestamp + kFrameIntervalNanos);
        env->SetLongField(timeline, gIds.timeline_deadline, timestamp + kFrameIntervalNanos / 2);
    }
    env->DeleteLocalRef(timeline);
    env->DeleteLocalRef(timelines);
}

void* VsyncThread(void*) {
    pthread_setname_np("a2i-vsync");
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, "a2i-vsync", nullptr};
    if (AndroidRuntime::getJavaVM()->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) {
        return nullptr;
    }
    jint frame = 0;
    for (;;) {
        usleep(static_cast<useconds_t>(kFrameIntervalNanos / 1000));
        std::vector<Receiver*> due;
        {
            std::lock_guard<std::mutex> lock(gLock);
            for (Receiver* receiver : Receivers()) {
                if (receiver->vsync_requested) {
                    receiver->vsync_requested = false;
                    due.push_back(receiver);
                }
            }
        }
        if (due.empty()) {
            continue;
        }
        frame++;
        jlong timestamp = env->CallStaticLongMethod(gIds.system_class, gIds.nano_time);
        for (Receiver* receiver : due) {
            jobject target = env->CallObjectMethod(receiver->receiver_weak, gIds.weak_get);
            jobject data = env->CallObjectMethod(receiver->vsync_data_weak, gIds.weak_get);
            if (target != nullptr) {
                if (data != nullptr) {
                    FillVsyncData(env, data, timestamp, frame);
                }
                env->CallVoidMethod(target, gIds.dispatch_vsync, timestamp, static_cast<jlong>(0), frame);
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe();
                    env->ExceptionClear();
                }
            }
            env->DeleteLocalRef(target);
            env->DeleteLocalRef(data);
        }
    }
}

void StartVsyncThread() {
    pthread_t thread;
    pthread_create(&thread, nullptr, VsyncThread, nullptr);
    pthread_detach(thread);
}

jlong nativeInit(JNIEnv* env, jclass, jobject receiverWeak, jobject vsyncDataWeak, jobject /*messageQueue*/,
                 jint /*vsyncSource*/, jint /*eventRegistration*/, jlong /*layerHandle*/) {
    auto* receiver = new Receiver();
    receiver->receiver_weak = env->NewGlobalRef(receiverWeak);
    receiver->vsync_data_weak = env->NewGlobalRef(vsyncDataWeak);
    {
        std::lock_guard<std::mutex> lock(gLock);
        Receivers().push_back(receiver);
    }
    pthread_once(&gThreadOnce, StartVsyncThread);
    return reinterpret_cast<jlong>(receiver);
}

void DestroyReceiver(Receiver* receiver) {
    {
        std::lock_guard<std::mutex> lock(gLock);
        auto& receivers = Receivers();
        for (auto it = receivers.begin(); it != receivers.end(); ++it) {
            if (*it == receiver) {
                receivers.erase(it);
                break;
            }
        }
    }
    if (JNIEnv* env = AndroidRuntime::getJNIEnv()) {
        env->DeleteGlobalRef(receiver->receiver_weak);
        env->DeleteGlobalRef(receiver->vsync_data_weak);
    }
    delete receiver;
}

jlong nativeGetDisplayEventReceiverFinalizer(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&DestroyReceiver);
}

void nativeScheduleVsync(JNIEnv*, jclass, jlong ptr) {
    std::lock_guard<std::mutex> lock(gLock);
    reinterpret_cast<Receiver*>(ptr)->vsync_requested = true;
}

jobject nativeGetLatestVsyncEventData(JNIEnv* env, jclass, jlong) {
    jclass clazz = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData");
    jobject data = env->NewObject(clazz, env->GetMethodID(clazz, "<init>", "()V"));
    if (data != nullptr) {
        FillVsyncData(env, data, env->CallStaticLongMethod(gIds.system_class, gIds.nano_time), 0);
    }
    return data;
}

const JNINativeMethod kMethods[] = {
        {"nativeInit",
         "(Ljava/lang/ref/WeakReference;Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;IIJ)J",
         (void*)nativeInit},
        {"nativeGetDisplayEventReceiverFinalizer", "()J", (void*)nativeGetDisplayEventReceiverFinalizer},
        {"nativeScheduleVsync", "(J)V", (void*)nativeScheduleVsync},
        {"nativeGetLatestVsyncEventData", "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;",
         (void*)nativeGetLatestVsyncEventData},
};

}  // namespace

int register_android_view_DisplayEventReceiver(JNIEnv* env) {
    jclass reference = env->FindClass("java/lang/ref/Reference");
    jclass receiver = env->FindClass("android/view/DisplayEventReceiver");
    jclass data = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData");
    jclass timeline = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData$FrameTimeline");
    jclass system = env->FindClass("java/lang/System");
    if (reference == nullptr || receiver == nullptr || data == nullptr || timeline == nullptr || system == nullptr) {
        return -1;
    }
    // Natives first: looking up a method initialises the class, and DisplayEventReceiver's
    // static initialiser already calls one of them.
    if (jniRegisterNativeMethods(env, "android/view/DisplayEventReceiver", kMethods, NELEM(kMethods)) < 0) {
        return -1;
    }
    gIds.weak_get = env->GetMethodID(reference, "get", "()Ljava/lang/Object;");
    gIds.dispatch_vsync = env->GetMethodID(receiver, "dispatchVsync", "(JJI)V");
    gIds.system_class = static_cast<jclass>(env->NewGlobalRef(system));
    gIds.nano_time = env->GetStaticMethodID(system, "nanoTime", "()J");
    gIds.frame_interval = env->GetFieldID(data, "frameInterval", "J");
    gIds.preferred_index = env->GetFieldID(data, "preferredFrameTimelineIndex", "I");
    gIds.timelines_length = env->GetFieldID(data, "frameTimelinesLength", "I");
    gIds.timelines = env->GetFieldID(
            data, "frameTimelines", "[Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;");
    gIds.timeline_vsync_id = env->GetFieldID(timeline, "vsyncId", "J");
    gIds.timeline_expected_presentation = env->GetFieldID(timeline, "expectedPresentationTime", "J");
    gIds.timeline_deadline = env->GetFieldID(timeline, "deadline", "J");
    return env->ExceptionCheck() ? -1 : 0;
}

}  // namespace android
