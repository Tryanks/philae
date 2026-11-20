// Natives of assorted framework classes whose work has no counterpart in this
// environment. Each one is the trivial, correct answer for a single-process Android
// without a dynamic linker of its own or a system server.

#include <stdlib.h>
#include <unistd.h>

#include <atomic>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

// com.android.internal.os.ClassLoaderFactory.createClassloaderNamespace: on a device this
// creates the linker namespace that confines an app's native libraries. iOS loads the
// app's libraries from its bundle with dlopen and has no namespaces. Null means success.
jstring ClassLoaderFactory_createClassloaderNamespace(JNIEnv*, jclass, jobject, jint, jstring, jstring,
                                                     jboolean, jstring, jstring) {
    return nullptr;
}

const JNINativeMethod kClassLoaderFactoryMethods[] = {
        {"createClassloaderNamespace",
         "(Ljava/lang/ClassLoader;ILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;)"
         "Ljava/lang/String;",
         (void*)ClassLoaderFactory_createClassloaderNamespace},
};

// android.view.SurfaceControl: handles to SurfaceFlinger layers and the transactions that
// change them. There is no compositor here (windows are drawn into bitmaps by
// A2IViewRoot), but framework classes create transaction objects as a matter of course,
// so those must be constructible. Their native peers are placeholders that only get
// allocated and freed; applying a transaction is not implemented.
void FreePlaceholder(void* placeholder) {
    free(placeholder);
}
void NoFinalizer(void*) {}

jlong SurfaceControl_placeholderFinalizer(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&FreePlaceholder);
}
jlong SurfaceControl_noFinalizer(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&NoFinalizer);
}
jlong SurfaceControl_nativeCreateTransaction(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(calloc(1, sizeof(void*)));
}
jobject SurfaceControl_nativeGetDefaultApplyToken(JNIEnv*, jclass) {
    return nullptr;
}
// Applying a transaction would hand the changes to the compositor. Windows here are
// composed by A2IWindowManager from what their views draw, so there is nothing to change.
void SurfaceControl_nativeApplyTransaction(JNIEnv*, jclass, jlong, jboolean, jboolean) {}

const JNINativeMethod kSurfaceControlMethods[] = {
        {"nativeGetNativeSurfaceControlFinalizer", "()J", (void*)SurfaceControl_noFinalizer},
        {"getNativeTrustedPresentationCallbackFinalizer", "()J", (void*)SurfaceControl_noFinalizer},
        {"nativeCreateTransaction", "()J", (void*)SurfaceControl_nativeCreateTransaction},
        {"nativeGetNativeTransactionFinalizer", "()J", (void*)SurfaceControl_placeholderFinalizer},
        {"nativeGetDefaultApplyToken", "()Landroid/os/IBinder;", (void*)SurfaceControl_nativeGetDefaultApplyToken},
        {"nativeApplyTransaction", "(JZZ)V", (void*)SurfaceControl_nativeApplyTransaction},
};

// android.view.SurfaceSession: a connection to the compositor; likewise a placeholder.
jlong SurfaceSession_nativeCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(calloc(1, sizeof(void*)));
}
void SurfaceSession_nativeDestroy(JNIEnv*, jclass, jlong ptr) {
    free(reinterpret_cast<void*>(ptr));
}

const JNINativeMethod kSurfaceSessionMethods[] = {
        {"nativeCreate", "()J", (void*)SurfaceSession_nativeCreate},
        {"nativeDestroy", "(J)V", (void*)SurfaceSession_nativeDestroy},
};

// android.os.GraphicsEnvironment.hintActivityLaunch: tells the GPU driver loader that an
// activity is about to start so it can warm up. There is no driver to load.
void GraphicsEnvironment_hintActivityLaunch(JNIEnv*, jclass) {}

const JNINativeMethod kGraphicsEnvironmentMethods[] = {
        {"hintActivityLaunch", "()V", (void*)GraphicsEnvironment_hintActivityLaunch},
};

// android.app.ActivityThread.nPurgePendingResources: releases GPU resources the render
// thread has queued for deletion. Rendering here is in software.
void ActivityThread_nPurgePendingResources(JNIEnv*, jobject) {}

const JNINativeMethod kActivityThreadMethods[] = {
        {"nPurgePendingResources", "()V", (void*)ActivityThread_nPurgePendingResources},
};

// android.os.FileObserver: notification of changes to files, from the kernel's inotify
// on Android. The interface is the kernel's: one descriptor that events are read from,
// and a watch descriptor per watched path. Watches can be set up and taken down here, but
// no events are delivered yet: nothing in a single-app system changes an app's files
// behind its back, which is what apps mostly watch for.
jint FileObserver_init(JNIEnv*, jobject) {
    int fds[2];
    return pipe(fds) == 0 ? fds[0] : -1;  // nothing is ever written to the other end
}

void FileObserver_observe(JNIEnv*, jobject, jint fd) {
    // The observer thread's loop: waits for events until the descriptor is closed.
    char buffer[64];
    while (read(fd, buffer, sizeof buffer) > 0) {
    }
}

void FileObserver_startWatching(JNIEnv* env, jobject, jint, jobjectArray paths, jint, jintArray watches) {
    static std::atomic<jint> next{1};
    jsize count = env->GetArrayLength(paths);
    for (jsize i = 0; i < count; i++) {
        jint watch = next++;
        env->SetIntArrayRegion(watches, i, 1, &watch);
    }
}

void FileObserver_stopWatching(JNIEnv*, jobject, jint, jintArray) {}

const JNINativeMethod kFileObserverMethods[] = {
        {"init", "()I", (void*)FileObserver_init},
        {"observe", "(I)V", (void*)FileObserver_observe},
        {"startWatching", "(I[Ljava/lang/String;I[I)V", (void*)FileObserver_startWatching},
        {"stopWatching", "(I[I)V", (void*)FileObserver_stopWatching},
};

// android.hardware.SystemSensorManager: the device's sensors, from the sensor service.
// The manager exists and reports that there are none (accelerometer and friends are not
// bridged to iOS's Core Motion yet), which apps must handle: plenty of devices lack one
// sensor or another.
void SensorManager_nativeClassInit(JNIEnv*, jclass) {}
jlong SensorManager_nativeCreate(JNIEnv*, jclass, jstring) {
    return 1;
}
jboolean SensorManager_noSensorAtIndex(JNIEnv*, jclass, jlong, jobject, jint) {
    return JNI_FALSE;
}
void SensorManager_noDynamicSensors(JNIEnv*, jclass, jlong, jobject) {}
void SensorManager_noRuntimeSensors(JNIEnv*, jclass, jlong, jint, jobject) {}
jboolean SensorManager_false(JNIEnv*, jclass, jlong) {
    return JNI_FALSE;
}

const JNINativeMethod kSensorManagerMethods[] = {
        {"nativeClassInit", "()V", (void*)SensorManager_nativeClassInit},
        {"nativeCreate", "(Ljava/lang/String;)J", (void*)SensorManager_nativeCreate},
        {"nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z", (void*)SensorManager_noSensorAtIndex},
        {"nativeGetDefaultDeviceSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
         (void*)SensorManager_noSensorAtIndex},
        {"nativeGetDynamicSensors", "(JLjava/util/List;)V", (void*)SensorManager_noDynamicSensors},
        {"nativeGetRuntimeSensors", "(JILjava/util/List;)V", (void*)SensorManager_noRuntimeSensors},
        {"nativeIsDataInjectionEnabled", "(J)Z", (void*)SensorManager_false},
        {"nativeIsReplayDataInjectionEnabled", "(J)Z", (void*)SensorManager_false},
        {"nativeIsHalBypassReplayDataInjectionEnabled", "(J)Z", (void*)SensorManager_false},
};

// android.os.incremental.IncrementalManager: Android's file system for apps that run
// while they are still downloading. Nothing is installed that way here.
jboolean IncrementalManager_false(JNIEnv*, jclass) {
    return JNI_FALSE;
}
jboolean IncrementalManager_notIncrementalPath(JNIEnv*, jclass, jstring) {
    return JNI_FALSE;
}
jboolean IncrementalManager_notIncrementalFd(JNIEnv*, jclass, jint) {
    return JNI_FALSE;
}
jbyteArray IncrementalManager_noFileSignature(JNIEnv*, jclass, jstring) {
    return nullptr;
}

const JNINativeMethod kIncrementalManagerMethods[] = {
        {"nativeIsEnabled", "()Z", (void*)IncrementalManager_false},
        {"nativeIsV2Available", "()Z", (void*)IncrementalManager_false},
        {"nativeIsIncrementalPath", "(Ljava/lang/String;)Z", (void*)IncrementalManager_notIncrementalPath},
        {"nativeIsIncrementalFd", "(I)Z", (void*)IncrementalManager_notIncrementalFd},
        {"nativeUnsafeGetFileSignature", "(Ljava/lang/String;)[B", (void*)IncrementalManager_noFileSignature},
};

}  // namespace

int register_a2i_misc_natives(JNIEnv* env) {
    if (jniRegisterNativeMethods(env, "com/android/internal/os/ClassLoaderFactory",
                                 kClassLoaderFactoryMethods, NELEM(kClassLoaderFactoryMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/app/ActivityThread", kActivityThreadMethods,
                                 NELEM(kActivityThreadMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/os/incremental/IncrementalManager", kIncrementalManagerMethods,
                                 NELEM(kIncrementalManagerMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/os/FileObserver$ObserverThread", kFileObserverMethods,
                                 NELEM(kFileObserverMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/hardware/SystemSensorManager", kSensorManagerMethods,
                                 NELEM(kSensorManagerMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/os/GraphicsEnvironment", kGraphicsEnvironmentMethods,
                                 NELEM(kGraphicsEnvironmentMethods)) < 0) {
        return -1;
    }
    if (jniRegisterNativeMethods(env, "android/view/SurfaceControl", kSurfaceControlMethods,
                                 NELEM(kSurfaceControlMethods)) < 0) {
        return -1;
    }
    return jniRegisterNativeMethods(env, "android/view/SurfaceSession", kSurfaceSessionMethods,
                                    NELEM(kSurfaceSessionMethods));
}

}  // namespace android
