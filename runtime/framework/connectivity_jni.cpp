// libframework-connectivity-tiramisu-jni: the native half of android.net.TrafficStats in
// Android's connectivity module. Its one job at that level is tagging sockets so the
// kernel can account their traffic per app and tag, which the framework does to every
// socket it opens. There is no such accounting to take part in on iOS; tagging succeeds
// and does nothing.

#include <nativehelper/JNIHelp.h>

namespace {

jint TrafficStats_tagSocketFd(JNIEnv*, jclass, jobject, jint, jint) {
    return 0;
}

jint TrafficStats_untagSocketFd(JNIEnv*, jclass, jobject) {
    return 0;
}

const JNINativeMethod kMethods[] = {
        {"native_tagSocketFd", "(Ljava/io/FileDescriptor;II)I", (void*)TrafficStats_tagSocketFd},
        {"native_untagSocketFd", "(Ljava/io/FileDescriptor;)I", (void*)TrafficStats_untagSocketFd},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    if (jniRegisterNativeMethods(env, "android/net/TrafficStats", kMethods, NELEM(kMethods)) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
