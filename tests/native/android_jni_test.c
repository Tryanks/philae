// JNI as an Android app's native library uses it, built with Android's ABI. Exercised by
// tests/native/NativeTest.java on the runtime; see tests/native/run.sh.

#include <jni.h>

typedef unsigned long size_t;
int snprintf(char*, size_t, const char*, ...);
int pthread_create(long*, const void*, void* (*)(void*), void*);
int pthread_join(long, void**);

static JavaVM* gVm;
static jclass gClass;  // a2i.nativetest.NativeTest, global reference

// --- Java calling native ------------------------------------------------------------------

JNIEXPORT jint JNICALL Java_a2i_nativetest_NativeTest_add(JNIEnv* env, jclass clazz, jint a, jint b) {
    return a + b;
}

JNIEXPORT jstring JNICALL Java_a2i_nativetest_NativeTest_describe(JNIEnv* env, jclass clazz, jstring text, jint n,
                                                                 jdouble d, jlong l) {
    const char* chars = (*env)->GetStringUTFChars(env, text, 0);
    char buffer[128];
    snprintf(buffer, sizeof buffer, "%s/%d/%.2f/%lld", chars, n, d, (long long)l);
    (*env)->ReleaseStringUTFChars(env, text, chars);
    return (*env)->NewStringUTF(env, buffer);
}

// Sixteen arguments: the ones that do not fit in registers arrive on the stack.
JNIEXPORT jdouble JNICALL Java_a2i_nativetest_NativeTest_manyArguments(
        JNIEnv* env, jclass clazz, jint a, jlong b, jfloat c, jdouble d, jint e, jlong f, jfloat g, jdouble h,
        jint i, jlong j, jfloat k, jdouble l, jint m, jbyte n, jshort o, jfloat p, jdouble q, jfloat r, jint s) {
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + s;
}

// An instance method registered from JNI_OnLoad rather than found by name.
static jint twice(JNIEnv* env, jobject self, jint value) {
    return value * 2;
}

// --- Native calling Java ------------------------------------------------------------------

static jint call_v(JNIEnv* env, jclass clazz, jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jint result = (*env)->CallStaticIntMethodV(env, clazz, method, arguments);
    va_end(arguments);
    return result;
}

// Returns 0, or the number of the first check that failed.
JNIEXPORT jint JNICALL Java_a2i_nativetest_NativeTest_callBack(JNIEnv* env, jclass clazz, jobject target) {
    jmethodID mix = (*env)->GetStaticMethodID(env, clazz, "mix", "(IJFDLjava/lang/String;ZBCS)I");
    jmethodID many = (*env)->GetStaticMethodID(env, clazz, "many", "(IIIIIIIIIIDDDDDDDDDD)D");
    jmethodID join = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, target), "join",
                                         "(Ljava/lang/String;I)Ljava/lang/String;");
    if (mix == 0 || many == 0 || join == 0) return 1;
    jstring text = (*env)->NewStringUTF(env, "abc");

    // Variable arguments, the Android way: in registers, then on the stack.
    if ((*env)->CallStaticIntMethod(env, clazz, mix, 1, (jlong)1 << 40, 2.5f, 3.25, text, JNI_TRUE, (jbyte)-3,
                                    (jchar)'x', (jshort)-7) != 77) {
        return 2;
    }
    // A va_list.
    if (call_v(env, clazz, mix, 1, (jlong)1 << 40, 2.5f, 3.25, text, JNI_TRUE, (jbyte)-3, (jchar)'x', (jshort)-7)
        != 77) {
        return 3;
    }
    // An argument array.
    jvalue values[9];
    values[0].i = 1;
    values[1].j = (jlong)1 << 40;
    values[2].f = 2.5f;
    values[3].d = 3.25;
    values[4].l = text;
    values[5].z = JNI_TRUE;
    values[6].b = -3;
    values[7].c = 'x';
    values[8].s = -7;
    if ((*env)->CallStaticIntMethodA(env, clazz, mix, values) != 77) return 4;

    jdouble sum = (*env)->CallStaticDoubleMethod(env, clazz, many, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0.5, 1.5, 2.5, 3.5,
                                                 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
    if (sum != 105.0) return 5;

    jstring joined = (jstring)(*env)->CallObjectMethod(env, target, join, text, 42);
    const char* chars = (*env)->GetStringUTFChars(env, joined, 0);
    int ok = chars[0] == 'a' && chars[3] == '4' && chars[4] == '2' && chars[5] == 0;
    (*env)->ReleaseStringUTFChars(env, joined, chars);
    if (!ok) return 6;

    // A constructor with arguments.
    jclass integer = (*env)->FindClass(env, "java/lang/Integer");
    jobject boxed = (*env)->NewObject(env, integer, (*env)->GetMethodID(env, integer, "<init>", "(I)V"), 1234);
    if ((*env)->CallIntMethod(env, boxed, (*env)->GetMethodID(env, integer, "intValue", "()I")) != 1234) return 7;
    return 0;
}

JNIEXPORT void JNICALL Java_a2i_nativetest_NativeTest_throwSomething(JNIEnv* env, jclass clazz) {
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), "thrown from native code");
}

// --- A thread the library creates itself ----------------------------------------------------

static void* thread_main(void* argument) {
    JNIEnv* env = 0;
    if ((*gVm)->AttachCurrentThread(gVm, &env, 0) != JNI_OK) return (void*)1;
    jmethodID report = (*env)->GetStaticMethodID(env, gClass, "fromThread", "(ILjava/lang/String;)I");
    jint result = (*env)->CallStaticIntMethod(env, gClass, report, 21, (*env)->NewStringUTF(env, "thread"));
    (*gVm)->DetachCurrentThread(gVm);
    return (void*)(long)result;
}

JNIEXPORT jint JNICALL Java_a2i_nativetest_NativeTest_runThread(JNIEnv* env, jclass clazz) {
    long thread;
    void* result = 0;
    if (pthread_create(&thread, 0, thread_main, 0) != 0) return -1;
    pthread_join(thread, &result);
    return (jint)(long)result;
}

// --- Loading ----------------------------------------------------------------------------------

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    gVm = vm;
    JNIEnv* env = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    JavaVM* again = 0;
    if ((*env)->GetJavaVM(env, &again) != JNI_OK || again != vm) return JNI_ERR;
    jclass clazz = (*env)->FindClass(env, "a2i/nativetest/NativeTest");
    if (clazz == 0) return JNI_ERR;
    gClass = (*env)->NewGlobalRef(env, clazz);
    static const JNINativeMethod methods[] = {{"twice", "(I)I", (void*)twice}};
    if ((*env)->RegisterNatives(env, clazz, methods, 1) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
