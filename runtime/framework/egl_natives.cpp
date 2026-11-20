// javax.microedition.khronos.egl (EGL 1.0 for Java), implemented by
// com.google.android.gles_jni.EGLImpl.
//
// There is no OpenGL ES here yet: windows are drawn in software, and iOS's own GL is not
// bridged. EGL can say so in its own terms: a display that initializes and has no
// configurations to render with. Apps that only probe the GPU's limits at startup get
// their answer ("nothing"), and creating a context fails with EGL_BAD_CONFIG, the error
// for "no such configuration".

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

constexpr jint EGL_SUCCESS = 0x3000;
constexpr jint EGL_BAD_CONFIG = 0x3005;
constexpr jlong kDisplay = 1;  // the one, default display

thread_local jint tError = EGL_SUCCESS;

void SetCount(JNIEnv* env, jintArray count, jint value) {
    if (count != nullptr && env->GetArrayLength(count) > 0) {
        env->SetIntArrayRegion(count, 0, 1, &value);
    }
}

void EGL_nativeClassInit(JNIEnv*, jclass) {}

jlong EGL_getDisplay(JNIEnv*, jobject, jobject) {
    return kDisplay;
}

jboolean EGL_initialize(JNIEnv* env, jobject, jobject, jintArray version) {
    if (version != nullptr && env->GetArrayLength(version) >= 2) {
        const jint values[] = {1, 4};
        env->SetIntArrayRegion(version, 0, 2, values);
    }
    tError = EGL_SUCCESS;
    return JNI_TRUE;
}

jboolean EGL_terminate(JNIEnv*, jobject, jobject) {
    return JNI_TRUE;
}

jboolean EGL_getConfigs(JNIEnv* env, jobject, jobject, jobjectArray, jint, jintArray count) {
    SetCount(env, count, 0);
    return JNI_TRUE;
}

jboolean EGL_chooseConfig(JNIEnv* env, jobject, jobject, jintArray, jobjectArray, jint, jintArray count) {
    SetCount(env, count, 0);
    return JNI_TRUE;
}

jboolean EGL_getConfigAttrib(JNIEnv*, jobject, jobject, jobject, jint, jintArray) {
    tError = EGL_BAD_CONFIG;
    return JNI_FALSE;
}

jint EGL_getError(JNIEnv*, jobject) {
    jint error = tError;
    tError = EGL_SUCCESS;
    return error;
}

jstring EGL_queryString(JNIEnv* env, jobject, jobject, jint name) {
    switch (name) {
        case 0x3053: return env->NewStringUTF("Android");  // EGL_VENDOR
        case 0x3054: return env->NewStringUTF("1.4");      // EGL_VERSION
        default: return env->NewStringUTF("");             // EGL_EXTENSIONS, EGL_CLIENT_APIS
    }
}

// Contexts and surfaces need a configuration, and there is none.
jlong EGL_createContext(JNIEnv*, jobject, jobject, jobject, jobject, jintArray) {
    tError = EGL_BAD_CONFIG;
    return 0;
}
jlong EGL_createPbufferSurface(JNIEnv*, jobject, jobject, jobject, jintArray) {
    tError = EGL_BAD_CONFIG;
    return 0;
}
jlong EGL_createWindowSurface(JNIEnv*, jobject, jobject, jobject, jobject, jintArray) {
    tError = EGL_BAD_CONFIG;
    return 0;
}
void EGL_createPixmapSurface(JNIEnv*, jobject, jobject, jobject, jobject, jobject, jintArray) {
    tError = EGL_BAD_CONFIG;
}
jboolean EGL_makeCurrent(JNIEnv*, jobject, jobject, jobject, jobject, jobject) {
    return JNI_TRUE;  // only "nothing current" can be asked for
}
jboolean EGL_destroy(JNIEnv*, jobject, jobject, jobject) {
    return JNI_TRUE;
}
jboolean EGL_query(JNIEnv*, jobject, jobject, jobject, jint, jintArray) {
    tError = EGL_BAD_CONFIG;
    return JNI_FALSE;
}
jboolean EGL_true(JNIEnv*, jobject) {
    return JNI_TRUE;
}
jboolean EGL_waitNative(JNIEnv*, jobject, jint, jobject) {
    return JNI_TRUE;
}
jboolean EGL_swapBuffers(JNIEnv*, jobject, jobject, jobject) {
    return JNI_FALSE;
}
jboolean EGL_copyBuffers(JNIEnv*, jobject, jobject, jobject, jobject) {
    return JNI_FALSE;
}
jlong EGL_noCurrent(JNIEnv*, jobject) {
    return 0;
}
jlong EGL_noCurrentSurface(JNIEnv*, jobject, jint) {
    return 0;
}
jint EGL_getInitCount(JNIEnv*, jclass, jobject) {
    return 1;
}

#define EGL "Ljavax/microedition/khronos/egl/"
const JNINativeMethod kMethods[] = {
        {"_nativeClassInit", "()V", (void*)EGL_nativeClassInit},
        {"_eglGetDisplay", "(Ljava/lang/Object;)J", (void*)EGL_getDisplay},
        {"eglInitialize", "(" EGL "EGLDisplay;[I)Z", (void*)EGL_initialize},
        {"eglTerminate", "(" EGL "EGLDisplay;)Z", (void*)EGL_terminate},
        {"eglGetConfigs", "(" EGL "EGLDisplay;[" EGL "EGLConfig;I[I)Z", (void*)EGL_getConfigs},
        {"eglChooseConfig", "(" EGL "EGLDisplay;[I[" EGL "EGLConfig;I[I)Z", (void*)EGL_chooseConfig},
        {"eglGetConfigAttrib", "(" EGL "EGLDisplay;" EGL "EGLConfig;I[I)Z", (void*)EGL_getConfigAttrib},
        {"eglGetError", "()I", (void*)EGL_getError},
        {"eglQueryString", "(" EGL "EGLDisplay;I)Ljava/lang/String;", (void*)EGL_queryString},
        {"_eglCreateContext", "(" EGL "EGLDisplay;" EGL "EGLConfig;" EGL "EGLContext;[I)J", (void*)EGL_createContext},
        {"_eglCreatePbufferSurface", "(" EGL "EGLDisplay;" EGL "EGLConfig;[I)J", (void*)EGL_createPbufferSurface},
        {"_eglCreateWindowSurface", "(" EGL "EGLDisplay;" EGL "EGLConfig;Ljava/lang/Object;[I)J",
         (void*)EGL_createWindowSurface},
        {"_eglCreateWindowSurfaceTexture", "(" EGL "EGLDisplay;" EGL "EGLConfig;Ljava/lang/Object;[I)J",
         (void*)EGL_createWindowSurface},
        {"_eglCreatePixmapSurface", "(" EGL "EGLSurface;" EGL "EGLDisplay;" EGL "EGLConfig;Ljava/lang/Object;[I)V",
         (void*)EGL_createPixmapSurface},
        {"eglMakeCurrent", "(" EGL "EGLDisplay;" EGL "EGLSurface;" EGL "EGLSurface;" EGL "EGLContext;)Z",
         (void*)EGL_makeCurrent},
        {"eglDestroyContext", "(" EGL "EGLDisplay;" EGL "EGLContext;)Z", (void*)EGL_destroy},
        {"eglDestroySurface", "(" EGL "EGLDisplay;" EGL "EGLSurface;)Z", (void*)EGL_destroy},
        {"eglQueryContext", "(" EGL "EGLDisplay;" EGL "EGLContext;I[I)Z", (void*)EGL_query},
        {"eglQuerySurface", "(" EGL "EGLDisplay;" EGL "EGLSurface;I[I)Z", (void*)EGL_query},
        {"eglReleaseThread", "()Z", (void*)EGL_true},
        {"eglWaitGL", "()Z", (void*)EGL_true},
        {"eglWaitNative", "(ILjava/lang/Object;)Z", (void*)EGL_waitNative},
        {"eglSwapBuffers", "(" EGL "EGLDisplay;" EGL "EGLSurface;)Z", (void*)EGL_swapBuffers},
        {"eglCopyBuffers", "(" EGL "EGLDisplay;" EGL "EGLSurface;Ljava/lang/Object;)Z", (void*)EGL_copyBuffers},
        {"_eglGetCurrentContext", "()J", (void*)EGL_noCurrent},
        {"_eglGetCurrentDisplay", "()J", (void*)EGL_noCurrent},
        {"_eglGetCurrentSurface", "(I)J", (void*)EGL_noCurrentSurface},
        {"getInitCount", "(" EGL "EGLDisplay;)I", (void*)EGL_getInitCount},
};
#undef EGL

// The GL object that goes with an EGL context. Only its class initializer is provided:
// with no context to make current, none of its drawing functions can be reached.
const JNINativeMethod kGlMethods[] = {
        {"_nativeClassInit", "()V", (void*)EGL_nativeClassInit},
};

}  // namespace

int register_com_google_android_gles_jni_EGLImpl(JNIEnv* env) {
    if (jniRegisterNativeMethods(env, "com/google/android/gles_jni/GLImpl", kGlMethods, NELEM(kGlMethods)) < 0) {
        return -1;
    }
    return jniRegisterNativeMethods(env, "com/google/android/gles_jni/EGLImpl", kMethods, NELEM(kMethods));
}

}  // namespace android
