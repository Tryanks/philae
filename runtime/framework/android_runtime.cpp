// Entry point of the framework's native code built by this project: the parts of
// Android's libandroid_runtime that AOSP's host build of that library (used for graphics,
// see scripts/prepare-prebuilt-runtime.sh) does not provide on Darwin.

#include <android_runtime/AndroidRuntime.h>

#include <nativehelper/JNIHelp.h>

namespace android {

// Declared in core/jni/android_util_Binder.h; its other functions need a Binder driver.
jobject newParcelFileDescriptor(JNIEnv* env, jobject fileDesc);

int register_android_content_AssetManager(JNIEnv* env);
int register_android_content_res_ApkAssets(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
int register_android_media_AudioSystem(JNIEnv* env);
int register_android_os_Binder(JNIEnv* env);
int register_android_os_MessageQueue(JNIEnv* env);
int register_android_os_Parcel(JNIEnv* env);
int register_android_os_SystemClock(JNIEnv* env);
int register_android_os_SystemProperties(JNIEnv* env);
int register_android_view_DisplayEventReceiver(JNIEnv* env);
int register_a2i_misc_natives(JNIEnv* env);
int register_com_google_android_gles_jni_EGLImpl(JNIEnv* env);

static JavaVM* gJavaVM;

int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                          const JNINativeMethod* gMethods, int numMethods) {
    return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}

JavaVM* AndroidRuntime::getJavaVM() {
    return gJavaVM;
}

JNIEnv* AndroidRuntime::getJNIEnv() {
    JNIEnv* env = nullptr;
    if (gJavaVM == nullptr || gJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return nullptr;
    }
    return env;
}

jobject newParcelFileDescriptor(JNIEnv* env, jobject fileDesc) {
    jclass clazz = env->FindClass("android/os/ParcelFileDescriptor");
    jmethodID constructor =
            clazz != nullptr ? env->GetMethodID(clazz, "<init>", "(Ljava/io/FileDescriptor;)V") : nullptr;
    return constructor != nullptr ? env->NewObject(clazz, constructor, fileDesc) : nullptr;
}

}  // namespace android

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    android::gJavaVM = vm;
    JNIEnv* env = android::AndroidRuntime::getJNIEnv();
    if (env == nullptr) {
        return JNI_ERR;
    }
    static int (*const kRegistrations[])(JNIEnv*) = {
            android::register_android_content_AssetManager,
            android::register_android_content_res_ApkAssets,
            android::register_android_content_StringBlock,
            android::register_android_content_XmlBlock,
            android::register_android_media_AudioSystem,
            android::register_android_os_Binder,
            android::register_android_os_MessageQueue,
            android::register_android_os_Parcel,
            android::register_android_os_SystemClock,
            android::register_android_os_SystemProperties,
            android::register_android_view_DisplayEventReceiver,
            android::register_a2i_misc_natives,
            android::register_com_google_android_gles_jni_EGLImpl,
    };
    for (auto registration : kRegistrations) {
        if (registration(env) < 0) {
            return JNI_ERR;
        }
    }
    return JNI_VERSION_1_6;
}
