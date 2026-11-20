// android.os.Binder and android.os.Process natives for a single-process Android.
//
// There is no Binder driver and no other process: every system service the app talks to
// is an object in this process (see a2i.boot's services), so a "transaction" is a plain
// method call and the caller is always the app itself. What remains of these classes'
// native side is identity (who is calling: this process) and bookkeeping.

#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

// --- android.os.Binder ---------------------------------------------------------------

// Each Java Binder owns a native peer on Android (a JavaBBinderHolder). Nothing needs one
// here, but the Java side registers it for cleanup, so it must be real, freeable memory.
jlong Binder_getNativeBBinderHolder(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(calloc(1, sizeof(void*)));
}

void DestroyHolder(void* holder) {
    free(holder);
}

jlong Binder_getNativeFinalizer(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&DestroyHolder);
}

jint Binder_getCallingPid(JNIEnv*, jclass) { return getpid(); }
jint Binder_getCallingUid(JNIEnv*, jclass) { return static_cast<jint>(getuid()); }
jboolean Binder_false(JNIEnv*, jclass) { return JNI_FALSE; }

// The identity token Android hands out encodes (uid << 32) | pid of the caller.
jlong Binder_clearCallingIdentity(JNIEnv*, jclass) {
    return (static_cast<jlong>(getuid()) << 32) | getpid();
}

void Binder_ignoreLong(JNIEnv*, jclass, jlong) {}
void Binder_ignore(JNIEnv*, jclass) {}

thread_local jint tStrictModePolicy = 0;
void Binder_setThreadStrictModePolicy(JNIEnv*, jclass, jint policy) { tStrictModePolicy = policy; }
jint Binder_getThreadStrictModePolicy(JNIEnv*, jclass) { return tStrictModePolicy; }

thread_local jint tWorkSourceUid = -1;
jlong Binder_setCallingWorkSourceUid(JNIEnv*, jclass, jint uid) {
    jlong token = tWorkSourceUid;
    tWorkSourceUid = uid;
    return token;
}
jint Binder_getCallingWorkSourceUid(JNIEnv*, jclass) { return tWorkSourceUid; }
jlong Binder_clearCallingWorkSource(JNIEnv*, jclass) {
    jlong token = tWorkSourceUid;
    tWorkSourceUid = -1;
    return token;
}
void Binder_restoreCallingWorkSource(JNIEnv*, jclass, jlong token) {
    tWorkSourceUid = static_cast<jint>(token);
}

void Binder_instanceNoop(JNIEnv*, jobject) {}
jobject Binder_getExtension(JNIEnv*, jobject) { return nullptr; }
void Binder_setExtension(JNIEnv*, jobject, jobject) {}

const JNINativeMethod kBinderMethods[] = {
        {"getNativeBBinderHolder", "()J", (void*)Binder_getNativeBBinderHolder},
        {"getNativeFinalizer", "()J", (void*)Binder_getNativeFinalizer},
        {"getCallingPid", "()I", (void*)Binder_getCallingPid},
        {"getCallingUid", "()I", (void*)Binder_getCallingUid},
        {"isDirectlyHandlingTransactionNative", "()Z", (void*)Binder_false},
        {"hasExplicitIdentity", "()Z", (void*)Binder_false},
        {"clearCallingIdentity", "()J", (void*)Binder_clearCallingIdentity},
        {"restoreCallingIdentity", "(J)V", (void*)Binder_ignoreLong},
        {"setThreadStrictModePolicy", "(I)V", (void*)Binder_setThreadStrictModePolicy},
        {"getThreadStrictModePolicy", "()I", (void*)Binder_getThreadStrictModePolicy},
        {"setCallingWorkSourceUid", "(I)J", (void*)Binder_setCallingWorkSourceUid},
        {"getCallingWorkSourceUid", "()I", (void*)Binder_getCallingWorkSourceUid},
        {"clearCallingWorkSource", "()J", (void*)Binder_clearCallingWorkSource},
        {"restoreCallingWorkSource", "(J)V", (void*)Binder_restoreCallingWorkSource},
        {"markVintfStability", "()V", (void*)Binder_instanceNoop},
        {"forceDowngradeToSystemStability", "()V", (void*)Binder_instanceNoop},
        {"flushPendingCommands", "()V", (void*)Binder_ignore},
        {"blockUntilThreadAvailable", "()V", (void*)Binder_ignore},
        {"getExtension", "()Landroid/os/IBinder;", (void*)Binder_getExtension},
        {"setExtension", "(Landroid/os/IBinder;)V", (void*)Binder_setExtension},
};

// --- android.os.Process --------------------------------------------------------------

jlong Process_getElapsedCpuTime(JNIEnv*, jclass) {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// Android thread priorities are Linux nice values; iOS schedules by QoS class and does
// not let an app renice its threads. Accept and remember nothing.
void Process_setThreadPriority(JNIEnv*, jclass, jint, jint) {}
void Process_setCallingThreadPriority(JNIEnv*, jclass, jint) {}
jint Process_getThreadPriority(JNIEnv*, jclass, jint) { return 0; }
void Process_setThreadGroup(JNIEnv*, jclass, jint, jint) {}
void Process_setArgV0(JNIEnv*, jclass, jstring) {}
void Process_setCanSelfBackground(JNIEnv*, jclass, jboolean) {}

// Process.killProcess and friends. The only process there is to signal is this one: an
// app ending itself, typically after its crash handler has run.
void Process_sendSignal(JNIEnv*, jclass, jint pid, jint signal) {
    if (pid == getpid()) {
        kill(pid, signal);
    }
}

const JNINativeMethod kProcessMethods[] = {
        {"sendSignal", "(II)V", (void*)Process_sendSignal},
        {"sendSignalQuiet", "(II)V", (void*)Process_sendSignal},
        {"getElapsedCpuTime", "()J", (void*)Process_getElapsedCpuTime},
        {"setThreadPriority", "(II)V", (void*)Process_setThreadPriority},
        {"setThreadPriority", "(I)V", (void*)Process_setCallingThreadPriority},
        {"getThreadPriority", "(I)I", (void*)Process_getThreadPriority},
        {"setThreadGroup", "(II)V", (void*)Process_setThreadGroup},
        {"setThreadGroupAndCpuset", "(II)V", (void*)Process_setThreadGroup},
        {"setProcessGroup", "(II)V", (void*)Process_setThreadGroup},
        {"setArgV0Native", "(Ljava/lang/String;)V", (void*)Process_setArgV0},
        {"setCanSelfBackground", "(Z)V", (void*)Process_setCanSelfBackground},
};

}  // namespace

int register_android_os_Binder(JNIEnv* env) {
    if (jniRegisterNativeMethods(env, "android/os/Binder", kBinderMethods, NELEM(kBinderMethods)) < 0) {
        return -1;
    }
    return jniRegisterNativeMethods(env, "android/os/Process", kProcessMethods, NELEM(kProcessMethods));
}

}  // namespace android
