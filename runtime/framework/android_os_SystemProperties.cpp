// android.os.SystemProperties: the system property store.
//
// On a device the properties live in shared memory managed by init. This process is the
// whole system, so the store is a map: filled from build.prop at start (see
// a2i.boot.Platform) and readable and writable through the usual API. AOSP's host
// implementation is not used because it lacks handle-based lookups (native_find), which
// the framework's generated flag and sysprop accessors rely on.

#include <stdlib.h>
#include <string.h>

#include <map>
#include <mutex>
#include <string>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedUtfChars.h>

namespace android {

namespace {

std::mutex gLock;
// std::map keeps node addresses stable, so a pointer to a value serves as its handle.
std::map<std::string, std::string>& Properties() {
    static auto* properties = new std::map<std::string, std::string>();
    return *properties;
}

bool Lookup(JNIEnv* env, jstring keyJ, std::string* value) {
    ScopedUtfChars key(env, keyJ);
    if (key.c_str() == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(gLock);
    auto it = Properties().find(key.c_str());
    if (it == Properties().end() || it->second.empty()) {
        return false;
    }
    *value = it->second;
    return true;
}

bool Lookup(jlong handle, std::string* value) {
    if (handle == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(gLock);
    *value = *reinterpret_cast<std::string*>(handle);
    return !value->empty();
}

// The same parsing rules as android::base::ParseInt / ParseBool on a device.
template <typename T>
T ParseInteger(const std::string& text, T fallback) {
    char* end = nullptr;
    long long parsed = strtoll(text.c_str(), &end, 0);
    return (end == text.c_str() || *end != '\0') ? fallback : static_cast<T>(parsed);
}

jboolean ParseBoolean(const std::string& text, jboolean fallback) {
    for (const char* yes : {"1", "y", "yes", "on", "true"}) {
        if (text == yes) return JNI_TRUE;
    }
    for (const char* no : {"0", "n", "no", "off", "false"}) {
        if (text == no) return JNI_FALSE;
    }
    return fallback;
}

jstring getByName(JNIEnv* env, jclass, jstring key, jstring fallback) {
    std::string value;
    return Lookup(env, key, &value) ? env->NewStringUTF(value.c_str()) : fallback;
}

jint getIntByName(JNIEnv* env, jclass, jstring key, jint fallback) {
    std::string value;
    return Lookup(env, key, &value) ? ParseInteger<jint>(value, fallback) : fallback;
}

jlong getLongByName(JNIEnv* env, jclass, jstring key, jlong fallback) {
    std::string value;
    return Lookup(env, key, &value) ? ParseInteger<jlong>(value, fallback) : fallback;
}

jboolean getBooleanByName(JNIEnv* env, jclass, jstring key, jboolean fallback) {
    std::string value;
    return Lookup(env, key, &value) ? ParseBoolean(value, fallback) : fallback;
}

jlong find(JNIEnv* env, jclass, jstring keyJ) {
    ScopedUtfChars key(env, keyJ);
    if (key.c_str() == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(gLock);
    auto it = Properties().find(key.c_str());
    return it == Properties().end() ? 0 : reinterpret_cast<jlong>(&it->second);
}

jstring getByHandle(JNIEnv* env, jclass, jlong handle) {
    std::string value;
    Lookup(handle, &value);
    return env->NewStringUTF(value.c_str());
}

jint getIntByHandle(JNIEnv*, jclass, jlong handle, jint fallback) {
    std::string value;
    return Lookup(handle, &value) ? ParseInteger<jint>(value, fallback) : fallback;
}

jlong getLongByHandle(JNIEnv*, jclass, jlong handle, jlong fallback) {
    std::string value;
    return Lookup(handle, &value) ? ParseInteger<jlong>(value, fallback) : fallback;
}

jboolean getBooleanByHandle(JNIEnv*, jclass, jlong handle, jboolean fallback) {
    std::string value;
    return Lookup(handle, &value) ? ParseBoolean(value, fallback) : fallback;
}

void set(JNIEnv* env, jclass, jstring keyJ, jstring valueJ) {
    ScopedUtfChars key(env, keyJ);
    if (key.c_str() == nullptr) {
        return;
    }
    std::string value;
    if (valueJ != nullptr) {
        ScopedUtfChars chars(env, valueJ);
        value = chars.c_str() != nullptr ? chars.c_str() : "";
    }
    std::lock_guard<std::mutex> lock(gLock);
    auto [it, inserted] = Properties().try_emplace(key.c_str(), value);
    if (inserted) {
        return;
    }
    if (strncmp(key.c_str(), "ro.", 3) == 0) {
        // Read-only properties can be defined once. (Unlocking before throwing is not
        // needed: the exception is only pending until this native returns.)
        jniThrowRuntimeException(env, "read-only system property is already set");
        return;
    }
    it->second = value;
}

void noop(JNIEnv*, jclass) {}

const JNINativeMethod kMethods[] = {
        {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*)getByName},
        {"native_get_int", "(Ljava/lang/String;I)I", (void*)getIntByName},
        {"native_get_long", "(Ljava/lang/String;J)J", (void*)getLongByName},
        {"native_get_boolean", "(Ljava/lang/String;Z)Z", (void*)getBooleanByName},
        {"native_find", "(Ljava/lang/String;)J", (void*)find},
        {"native_get", "(J)Ljava/lang/String;", (void*)getByHandle},
        {"native_get_int", "(JI)I", (void*)getIntByHandle},
        {"native_get_long", "(JJ)J", (void*)getLongByHandle},
        {"native_get_boolean", "(JZ)Z", (void*)getBooleanByHandle},
        {"native_set", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)set},
        // Change callbacks: nothing outside this process ever changes a property.
        {"native_add_change_callback", "()V", (void*)noop},
        {"native_report_sysprop_change", "()V", (void*)noop},
};

}  // namespace

int register_android_os_SystemProperties(JNIEnv* env) {
    return jniRegisterNativeMethods(env, "android/os/SystemProperties", kMethods, NELEM(kMethods));
}

}  // namespace android
