// android.os.SystemClock.
//
// All of Android's interval clocks are the same monotonic clock here, and it is the one
// System.nanoTime() reads: Choreographer and Handler mix the two freely, so they must
// share a time base. (AOSP's host implementation falls back to the wall clock outside
// Linux, which breaks that.) The device distinction between uptime, which stops in deep
// sleep, and elapsed real time, which does not, is not modelled.

#include <time.h>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

jlong ClockNanos(clockid_t clock) {
    timespec ts;
    clock_gettime(clock, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

jlong uptimeMillis(JNIEnv*, jclass) { return ClockNanos(CLOCK_MONOTONIC) / 1000000; }
jlong uptimeNanos(JNIEnv*, jclass) { return ClockNanos(CLOCK_MONOTONIC); }
jlong currentThreadTimeMillis(JNIEnv*, jclass) { return ClockNanos(CLOCK_THREAD_CPUTIME_ID) / 1000000; }
jlong currentThreadTimeMicro(JNIEnv*, jclass) { return ClockNanos(CLOCK_THREAD_CPUTIME_ID) / 1000; }
jlong currentTimeMicro(JNIEnv*, jclass) { return ClockNanos(CLOCK_REALTIME) / 1000; }

const JNINativeMethod kMethods[] = {
        {"uptimeMillis", "()J", (void*)uptimeMillis},
        {"uptimeNanos", "()J", (void*)uptimeNanos},
        {"elapsedRealtime", "()J", (void*)uptimeMillis},
        {"elapsedRealtimeNanos", "()J", (void*)uptimeNanos},
        {"currentThreadTimeMillis", "()J", (void*)currentThreadTimeMillis},
        {"currentThreadTimeMicro", "()J", (void*)currentThreadTimeMicro},
        {"currentTimeMicro", "()J", (void*)currentTimeMicro},
};

}  // namespace

int register_android_os_SystemClock(JNIEnv* env) {
    return jniRegisterNativeMethods(env, "android/os/SystemClock", kMethods, NELEM(kMethods));
}

}  // namespace android
