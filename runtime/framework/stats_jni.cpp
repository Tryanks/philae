// libstats_jni: android.util.StatsLog's native half, which on a device writes metrics
// atoms to the statsd daemon. There is no statsd here; the framework reports atoms as a
// side effect of ordinary work (every click, for one), so they are accepted and dropped.

#include <nativehelper/JNIHelp.h>

namespace {

void StatsLog_writeImpl(JNIEnv*, jclass, jbyteArray, jint, jint) {}

const JNINativeMethod kMethods[] = {
        {"writeImpl", "([BII)V", (void*)StatsLog_writeImpl},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    if (jniRegisterNativeMethods(env, "android/util/StatsLog", kMethods, NELEM(kMethods)) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
