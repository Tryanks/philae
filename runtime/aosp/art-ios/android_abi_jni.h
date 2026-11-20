// JNI for native code built for Android.
//
// An app's own native libraries are Android binaries (converted from ELF by the packer),
// and call JNI with Android's calling convention. For almost every JNI function that is
// the same as iOS's. The exception is variable arguments: `CallVoidMethod(env, obj, mid,
// ...)` passes them in registers on Android and on the stack on iOS, and the va_list of
// `CallVoidMethodV` is a different type. So Android code gets its own JNIEnv and JavaVM,
// whose function tables forward to the real ones, converting where the conventions
// differ.

#ifndef ART_IOS_ANDROID_ABI_JNI_H_
#define ART_IOS_ANDROID_ABI_JNI_H_

#include <jni.h>

namespace art {

// Whether `code` is in a library converted from an Android binary.
bool IsAndroidAbiCode(const void* code);

// The JNIEnv to hand to Android code running on the calling thread, given the real one.
JNIEnv* AndroidAbiJniEnv(JNIEnv* real);

// The JavaVM to hand to Android code.
JavaVM* AndroidAbiJavaVm(JavaVM* real);

}  // namespace art

#endif  // ART_IOS_ANDROID_ABI_JNI_H_
