// android::AndroidRuntime as the framework's JNI code sees it.
//
// On a device this class is the process's whole runtime bootstrap (it starts the VM in
// app_process). Here the VM is already running when the framework's native code loads,
// so only the accessors that JNI glue uses remain; see framework/android_runtime.cpp.
#pragma once

#include <jni.h>

namespace android {

class AndroidRuntime {
  public:
    static int registerNativeMethods(JNIEnv* env, const char* className,
                                     const JNINativeMethod* gMethods, int numMethods);
    static JavaVM* getJavaVM();
    static JNIEnv* getJNIEnv();
};

}  // namespace android
