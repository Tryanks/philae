// android.os.MessageQueue's native half: the blocking wait at the bottom of every Looper.
//
// AOSP implements it with libutils' Looper, which is built on epoll and eventfd. Messages
// themselves are queued in Java; the native side only has to sleep until a deadline or
// until another thread wakes it, which a mutex and condition variable provide.
// File descriptor callbacks (nativeSetFileDescriptorEvents) are not supported yet.

#include <pthread.h>
#include <time.h>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

struct NativeMessageQueue {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    bool woken = false;    // a wake arrived since the last poll returned
    bool polling = false;  // a thread is blocked in pollOnce
};

NativeMessageQueue* Queue(jlong ptr) {
    return reinterpret_cast<NativeMessageQueue*>(ptr);
}

jlong nativeInit(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(new NativeMessageQueue());
}

void nativeDestroy(JNIEnv*, jclass, jlong ptr) {
    delete Queue(ptr);
}

void nativePollOnce(JNIEnv*, jobject, jlong ptr, jint timeoutMillis) {
    NativeMessageQueue* queue = Queue(ptr);
    pthread_mutex_lock(&queue->mutex);
    if (!queue->woken && timeoutMillis != 0) {
        queue->polling = true;
        if (timeoutMillis < 0) {
            while (!queue->woken) {
                pthread_cond_wait(&queue->condition, &queue->mutex);
            }
        } else {
            timespec timeout = {timeoutMillis / 1000, (timeoutMillis % 1000) * 1000000L};
            // One wait is enough: returning early only makes the Looper poll again.
            pthread_cond_timedwait_relative_np(&queue->condition, &queue->mutex, &timeout);
        }
        queue->polling = false;
    }
    queue->woken = false;
    pthread_mutex_unlock(&queue->mutex);
}

void nativeWake(JNIEnv*, jclass, jlong ptr) {
    NativeMessageQueue* queue = Queue(ptr);
    pthread_mutex_lock(&queue->mutex);
    queue->woken = true;
    pthread_cond_signal(&queue->condition);
    pthread_mutex_unlock(&queue->mutex);
}

jboolean nativeIsPolling(JNIEnv*, jclass, jlong ptr) {
    NativeMessageQueue* queue = Queue(ptr);
    pthread_mutex_lock(&queue->mutex);
    bool polling = queue->polling;
    pthread_mutex_unlock(&queue->mutex);
    return polling;
}

void nativeSetFileDescriptorEvents(JNIEnv* env, jclass, jlong, jint, jint) {
    jniThrowException(env, "java/lang/UnsupportedOperationException",
                      "MessageQueue file descriptor events are not implemented");
}

const JNINativeMethod kMethods[] = {
        {"nativeInit", "()J", (void*)nativeInit},
        {"nativeDestroy", "(J)V", (void*)nativeDestroy},
        {"nativePollOnce", "(JI)V", (void*)nativePollOnce},
        {"nativeWake", "(J)V", (void*)nativeWake},
        {"nativeIsPolling", "(J)Z", (void*)nativeIsPolling},
        {"nativeSetFileDescriptorEvents", "(JII)V", (void*)nativeSetFileDescriptorEvents},
};

}  // namespace

int register_android_os_MessageQueue(JNIEnv* env) {
    return jniRegisterNativeMethods(env, "android/os/MessageQueue", kMethods, NELEM(kMethods));
}

}  // namespace android
