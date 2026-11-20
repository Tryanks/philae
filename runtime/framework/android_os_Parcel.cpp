// android.os.Parcel natives for a single-process Android.
//
// A Parcel is the container Binder transactions carry: a byte buffer of flattened values,
// in which Binder objects and file descriptors are special entries that the kernel
// translates between processes. Here nothing crosses a process boundary, so a parcel is
// only ever read by the process that wrote it. This implementation keeps what apps and
// the framework rely on:
//
//  - the layout of plain data is libbinder's (32-bit aligned little-endian values, strings
//    and arrays prefixed by their length), so marshall() and unmarshall() exchange the
//    same bytes as on a device;
//  - a Binder written to a parcel is read back as the same Java object, and a file
//    descriptor as a duplicate, which are the semantics of a transaction to oneself.

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <nativehelper/JNIHelp.h>

namespace android {

namespace {

// libbinder's status codes, as far as Parcel.java distinguishes them.
const jint kOk = 0;
const jint kNoMemory = -ENOMEM;
const jint kBadValue = -EINVAL;
const jint kNotEnoughData = -ENODATA;
const jint kFdsNotAllowed = static_cast<jint>(0x80000000 + 7);

// The marker that starts an interface token, as in libbinder ('SYST').
const int32_t kInterfaceHeader = ('S' << 24) | ('Y' << 16) | ('S' << 8) | 'T';

// Size of the entry a Binder or file descriptor occupies in the data: that of the
// kernel's flat_binder_object. Its contents are meaningless here; the object itself is
// kept beside the data, keyed by the entry's position.
const size_t kObjectSize = 24;

std::atomic<jlong> gAllocCount{0};
std::atomic<jlong> gAllocSize{0};

JavaVM* gVM;
jclass gFileDescriptorClass;
jmethodID gFileDescriptorInit;
jmethodID gFileDescriptorGetInt;
jmethodID gFileDescriptorSetInt;

size_t Pad4(size_t size) {
    return (size + 3) & ~static_cast<size_t>(3);
}

struct Object {
    jobject binder = nullptr;  // global reference; null for a null binder or a descriptor
    int fd = -1;               // owned; -1 for a binder
};

class Parcel {
public:
    Parcel() { gAllocCount++; }
    ~Parcel() { gAllocCount--; }

    size_t size() const { return mData.size(); }
    size_t position() const { return mPosition; }
    size_t capacity() const { return mData.capacity(); }
    size_t available() const { return mPosition < mData.size() ? mData.size() - mPosition : 0; }
    const uint8_t* data() const { return mData.data(); }
    bool allowFds = true;

    void setPosition(size_t position) { mPosition = position; }

    void reserve(size_t capacity) { mData.reserve(capacity); }

    void setSize(JNIEnv* env, size_t size) {
        if (size < mData.size()) {
            releaseObjects(env, size, mData.size());
        }
        mData.resize(size);
        if (mPosition > size) mPosition = size;
    }

    void clear(JNIEnv* env) {
        releaseObjects(env, 0, mData.size());
        mData.clear();
        mData.shrink_to_fit();
        mPosition = 0;
        allowFds = true;
    }

    // Writes bytes at the current position, padded to a 32-bit boundary.
    void write(JNIEnv* env, const void* bytes, size_t length) {
        size_t padded = Pad4(length);
        size_t end = mPosition + padded;
        if (end > mData.size()) {
            mData.resize(end);
        } else {
            releaseObjects(env, mPosition, end);  // overwritten
        }
        if (length > 0) memcpy(mData.data() + mPosition, bytes, length);
        if (padded > length) memset(mData.data() + mPosition + length, 0, padded - length);
        mPosition = end;
    }

    template <typename T>
    void writeValue(JNIEnv* env, T value) {
        write(env, &value, sizeof value);
    }

    // Reads `length` bytes (advancing by the padded length), or returns null if the data ends first.
    const uint8_t* read(size_t length) {
        size_t padded = Pad4(length);
        if (padded < length || padded > available()) return nullptr;
        const uint8_t* bytes = mData.data() + mPosition;
        mPosition += padded;
        return bytes;
    }

    template <typename T>
    T readValue() {
        T value{};
        if (const uint8_t* bytes = read(sizeof value)) memcpy(&value, bytes, sizeof value);
        return value;
    }

    void writeObject(JNIEnv* env, Object object) {
        size_t at = mPosition;
        uint8_t entry[kObjectSize] = {};
        write(env, entry, sizeof entry);
        mObjects[at] = object;
    }

    // The object entry at the current position, or null if there is none.
    const Object* readObject() {
        auto found = mObjects.find(mPosition);
        if (found == mObjects.end() || read(kObjectSize) == nullptr) return nullptr;
        return &found->second;
    }

    bool hasObjects(size_t from, size_t to, bool descriptors) const {
        for (auto i = mObjects.lower_bound(from); i != mObjects.end() && i->first < to; ++i) {
            if ((i->second.fd >= 0) == descriptors) return true;
        }
        return false;
    }

    bool hasAnyObjects() const { return !mObjects.empty(); }

    // Appends a range of another parcel, objects included, at the current position.
    jint appendFrom(JNIEnv* env, const Parcel& other, size_t offset, size_t length) {
        if (offset > other.size() || length > other.size() - offset) return kBadValue;
        size_t base = mPosition;
        write(env, other.data() + offset, length);
        for (auto i = other.mObjects.lower_bound(offset); i != other.mObjects.end(); ++i) {
            if (i->first + kObjectSize > offset + length) break;
            Object copy;
            if (i->second.fd >= 0) {
                copy.fd = fcntl(i->second.fd, F_DUPFD_CLOEXEC, 0);
            } else if (i->second.binder != nullptr) {
                copy.binder = env->NewGlobalRef(i->second.binder);
            }
            mObjects[base + (i->first - offset)] = copy;
        }
        return kOk;
    }

    void setData(JNIEnv* env, const uint8_t* bytes, size_t length) {
        clear(env);
        mData.assign(bytes, bytes + length);
    }

private:
    void releaseObjects(JNIEnv* env, size_t from, size_t to) {
        // An object is released when any part of its entry is in the range.
        size_t first = from >= kObjectSize ? from - kObjectSize + 1 : 0;
        for (auto i = mObjects.lower_bound(first); i != mObjects.end() && i->first < to;) {
            if (i->second.binder != nullptr) {
                // (Callers that are @CriticalNative have no JNIEnv to pass.)
                if (env == nullptr) gVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
                env->DeleteGlobalRef(i->second.binder);
            }
            if (i->second.fd >= 0) close(i->second.fd);
            i = mObjects.erase(i);
        }
    }

    std::vector<uint8_t> mData;
    size_t mPosition = 0;
    std::map<size_t, Object> mObjects;  // by position of the entry in mData
};

Parcel* P(jlong ptr) {
    return reinterpret_cast<Parcel*>(ptr);
}

// --- Bookkeeping (@CriticalNative methods take no JNIEnv or class) ----------------------

jlong Parcel_create(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(new Parcel());
}

void Parcel_freeBuffer(JNIEnv* env, jclass, jlong ptr) {
    P(ptr)->clear(env);
}

void Parcel_destroy(JNIEnv* env, jclass, jlong ptr) {
    P(ptr)->clear(env);
    delete P(ptr);
}

void Parcel_markSensitive(jlong) {}
void Parcel_markForBinder(JNIEnv*, jclass, jlong, jobject) {}
jboolean Parcel_isForRpc(jlong) { return JNI_FALSE; }
jint Parcel_dataSize(jlong ptr) { return static_cast<jint>(P(ptr)->size()); }
jint Parcel_dataAvail(jlong ptr) { return static_cast<jint>(P(ptr)->available()); }
jint Parcel_dataPosition(jlong ptr) { return static_cast<jint>(P(ptr)->position()); }
jint Parcel_dataCapacity(jlong ptr) { return static_cast<jint>(P(ptr)->capacity()); }

void Parcel_setDataSize(JNIEnv* env, jclass, jlong ptr, jint size) {
    if (size < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "negative size");
        return;
    }
    P(ptr)->setSize(env, static_cast<size_t>(size));
}

void Parcel_setDataPosition(jlong ptr, jint position) {
    P(ptr)->setPosition(position < 0 ? 0 : static_cast<size_t>(position));
}

void Parcel_setDataCapacity(JNIEnv*, jclass, jlong ptr, jint capacity) {
    if (capacity > 0) P(ptr)->reserve(static_cast<size_t>(capacity));
}

jboolean Parcel_pushAllowFds(jlong ptr, jboolean allow) {
    jboolean previous = P(ptr)->allowFds;
    if (!allow) P(ptr)->allowFds = false;
    return previous;
}

void Parcel_restoreAllowFds(jlong ptr, jboolean previous) {
    P(ptr)->allowFds = previous;
}

jboolean Parcel_replaceCallingWorkSourceUid(jlong, jint) { return JNI_FALSE; }
jint Parcel_readCallingWorkSourceUid(jlong) { return -1; }
jlong Parcel_getOpenAshmemSize(jlong) { return 0; }
jlong Parcel_getGlobalAllocSize(JNIEnv*, jclass) { return gAllocSize; }
jlong Parcel_getGlobalAllocCount(JNIEnv*, jclass) { return gAllocCount; }

void Parcel_signalExceptionForError(JNIEnv* env, jclass, jint error) {
    switch (error) {
        case kOk:
            return;
        case kNoMemory:
            jniThrowException(env, "java/lang/OutOfMemoryError", nullptr);
            return;
        case kBadValue:
            jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
            return;
        case kNotEnoughData:
            jniThrowException(env, "android/os/ParcelFormatException", "Not enough data");
            return;
        case kFdsNotAllowed:
            jniThrowException(env, "java/lang/RuntimeException",
                              "Not allowed to write file descriptors here");
            return;
        default:
            jniThrowException(env, "java/lang/RuntimeException",
                              ("Parcel error " + std::to_string(error)).c_str());
    }
}

// --- Plain values --------------------------------------------------------------------

// The four writers are @CriticalNative and so have no JNIEnv; the parcel looks one up in
// the rare case that it needs it (a value written over a Binder entry).
template <typename T>
jint WriteValue(jlong ptr, T value) {
    P(ptr)->writeValue(nullptr, value);
    return kOk;
}

jint Parcel_writeInt(jlong ptr, jint value) { return WriteValue(ptr, value); }
jint Parcel_writeLong(jlong ptr, jlong value) { return WriteValue(ptr, value); }
jint Parcel_writeFloat(jlong ptr, jfloat value) { return WriteValue(ptr, value); }
jint Parcel_writeDouble(jlong ptr, jdouble value) { return WriteValue(ptr, value); }
jint Parcel_readInt(jlong ptr) { return P(ptr)->readValue<jint>(); }
jlong Parcel_readLong(jlong ptr) { return P(ptr)->readValue<jlong>(); }
jfloat Parcel_readFloat(jlong ptr) { return P(ptr)->readValue<jfloat>(); }
jdouble Parcel_readDouble(jlong ptr) { return P(ptr)->readValue<jdouble>(); }

// Byte arrays: length, then the bytes. A null array is length -1.
void Parcel_writeByteArray(JNIEnv* env, jclass, jlong ptr, jbyteArray array, jint offset, jint length) {
    Parcel* parcel = P(ptr);
    if (array == nullptr) {
        parcel->writeValue<int32_t>(env, -1);
        return;
    }
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(array, offset, length, bytes.data());
    if (env->ExceptionCheck()) return;
    parcel->writeValue<int32_t>(env, length);
    parcel->write(env, bytes.data(), bytes.size());
}

// Blobs: like byte arrays, with a marker saying the data is stored in place (the other
// possibility on a device being a shared memory region, for large blobs).
void Parcel_writeBlob(JNIEnv* env, jclass clazz, jlong ptr, jbyteArray array, jint offset, jint length) {
    if (array == nullptr) {
        P(ptr)->writeValue<int32_t>(env, -1);
        return;
    }
    P(ptr)->writeValue<int32_t>(env, length);
    P(ptr)->writeValue<int32_t>(env, 0 /* BLOB_INPLACE */);
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(array, offset, length, bytes.data());
    if (env->ExceptionCheck()) return;
    P(ptr)->write(env, bytes.data(), bytes.size());
    (void)clazz;
}

jbyteArray ReadBytes(JNIEnv* env, Parcel* parcel, int32_t length) {
    if (length < 0 || static_cast<size_t>(length) > parcel->available()) return nullptr;
    const uint8_t* bytes = parcel->read(static_cast<size_t>(length));
    if (bytes == nullptr) return nullptr;
    jbyteArray array = env->NewByteArray(length);
    if (array != nullptr) env->SetByteArrayRegion(array, 0, length, reinterpret_cast<const jbyte*>(bytes));
    return array;
}

jbyteArray Parcel_createByteArray(JNIEnv* env, jclass, jlong ptr) {
    Parcel* parcel = P(ptr);
    return ReadBytes(env, parcel, parcel->readValue<int32_t>());
}

jboolean Parcel_readByteArray(JNIEnv* env, jclass, jlong ptr, jbyteArray dest, jint destLength) {
    Parcel* parcel = P(ptr);
    int32_t length = parcel->readValue<int32_t>();
    if (length < 0 || length != destLength || static_cast<size_t>(length) > parcel->available()) {
        return JNI_FALSE;
    }
    const uint8_t* bytes = parcel->read(static_cast<size_t>(length));
    if (bytes == nullptr) return JNI_FALSE;
    env->SetByteArrayRegion(dest, 0, length, reinterpret_cast<const jbyte*>(bytes));
    return JNI_TRUE;
}

jbyteArray Parcel_readBlob(JNIEnv* env, jclass, jlong ptr) {
    Parcel* parcel = P(ptr);
    int32_t length = parcel->readValue<int32_t>();
    if (length < 0) return nullptr;
    parcel->readValue<int32_t>();  // BLOB_INPLACE
    return ReadBytes(env, parcel, length);
}

// Strings: length in code units, the characters, and a terminator.
void Parcel_writeString16(JNIEnv* env, jclass, jlong ptr, jstring string) {
    Parcel* parcel = P(ptr);
    if (string == nullptr) {
        parcel->writeValue<int32_t>(env, -1);
        return;
    }
    jsize length = env->GetStringLength(string);
    std::vector<jchar> chars(static_cast<size_t>(length) + 1);
    env->GetStringRegion(string, 0, length, chars.data());
    parcel->writeValue<int32_t>(env, length);
    parcel->write(env, chars.data(), chars.size() * sizeof(jchar));
}

jstring Parcel_readString16(JNIEnv* env, jclass, jlong ptr) {
    Parcel* parcel = P(ptr);
    int32_t length = parcel->readValue<int32_t>();
    if (length < 0 || static_cast<size_t>(length) > parcel->available() / sizeof(jchar)) return nullptr;
    const uint8_t* bytes = parcel->read((static_cast<size_t>(length) + 1) * sizeof(jchar));
    if (bytes == nullptr) return nullptr;
    std::vector<jchar> chars(static_cast<size_t>(length));
    if (length > 0) memcpy(chars.data(), bytes, chars.size() * sizeof(jchar));
    return env->NewString(chars.data(), length);
}

void Parcel_writeString8(JNIEnv* env, jclass, jlong ptr, jstring string) {
    Parcel* parcel = P(ptr);
    if (string == nullptr) {
        parcel->writeValue<int32_t>(env, -1);
        return;
    }
    const char* chars = env->GetStringUTFChars(string, nullptr);
    size_t length = strlen(chars);
    parcel->writeValue<int32_t>(env, static_cast<int32_t>(length));
    parcel->write(env, chars, length + 1);
    env->ReleaseStringUTFChars(string, chars);
}

jstring Parcel_readString8(JNIEnv* env, jclass, jlong ptr) {
    Parcel* parcel = P(ptr);
    int32_t length = parcel->readValue<int32_t>();
    if (length < 0 || static_cast<size_t>(length) > parcel->available()) return nullptr;
    const uint8_t* bytes = parcel->read(static_cast<size_t>(length) + 1);
    if (bytes == nullptr) return nullptr;
    std::string chars(reinterpret_cast<const char*>(bytes), static_cast<size_t>(length));
    return env->NewStringUTF(chars.c_str());
}

// An interface token starts a transaction: the caller's strict mode policy and work
// source, a marker, and the name of the interface being called.
void Parcel_writeInterfaceToken(JNIEnv* env, jclass clazz, jlong ptr, jstring name) {
    Parcel* parcel = P(ptr);
    parcel->writeValue<int32_t>(env, 0);   // strict mode policy
    parcel->writeValue<int32_t>(env, -1);  // work source uid
    parcel->writeValue<int32_t>(env, kInterfaceHeader);
    Parcel_writeString16(env, clazz, ptr, name);
}

void Parcel_enforceInterface(JNIEnv* env, jclass clazz, jlong ptr, jstring name) {
    Parcel* parcel = P(ptr);
    parcel->readValue<int32_t>();
    parcel->readValue<int32_t>();
    int32_t header = parcel->readValue<int32_t>();
    jstring actual = header == kInterfaceHeader ? Parcel_readString16(env, clazz, ptr) : nullptr;
    if (actual == nullptr || name == nullptr) {
        jniThrowException(env, "java/lang/SecurityException", "Binder invocation to an incorrect interface");
        return;
    }
    jsize length = env->GetStringLength(name);
    bool same = length == env->GetStringLength(actual);
    if (same && length > 0) {
        std::vector<jchar> a(static_cast<size_t>(length)), b(static_cast<size_t>(length));
        env->GetStringRegion(name, 0, length, a.data());
        env->GetStringRegion(actual, 0, length, b.data());
        same = memcmp(a.data(), b.data(), a.size() * sizeof(jchar)) == 0;
    }
    if (!same) {
        jniThrowException(env, "java/lang/SecurityException", "Binder invocation to an incorrect interface");
    }
}

// --- Binders and file descriptors ------------------------------------------------------

void Parcel_writeStrongBinder(JNIEnv* env, jclass, jlong ptr, jobject binder) {
    Object object;
    object.binder = binder != nullptr ? env->NewGlobalRef(binder) : nullptr;
    P(ptr)->writeObject(env, object);
}

jobject Parcel_readStrongBinder(JNIEnv* env, jclass, jlong ptr) {
    const Object* object = P(ptr)->readObject();
    return object != nullptr && object->binder != nullptr ? env->NewLocalRef(object->binder) : nullptr;
}

void Parcel_writeFileDescriptor(JNIEnv* env, jclass, jlong ptr, jobject descriptor) {
    Parcel* parcel = P(ptr);
    if (!parcel->allowFds) {
        Parcel_signalExceptionForError(env, nullptr, kFdsNotAllowed);
        return;
    }
    int fd = descriptor != nullptr ? env->CallIntMethod(descriptor, gFileDescriptorGetInt) : -1;
    Object object;
    object.fd = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (object.fd < 0) {
        jniThrowException(env, "java/lang/RuntimeException", "cannot duplicate the file descriptor");
        return;
    }
    parcel->writeObject(env, object);
}

jobject Parcel_readFileDescriptor(JNIEnv* env, jclass, jlong ptr) {
    const Object* object = P(ptr)->readObject();
    if (object == nullptr || object->fd < 0) return nullptr;
    int fd = fcntl(object->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return nullptr;
    jobject descriptor = env->NewObject(gFileDescriptorClass, gFileDescriptorInit);
    if (descriptor == nullptr) {
        close(fd);
        return nullptr;
    }
    env->CallVoidMethod(descriptor, gFileDescriptorSetInt, fd);
    return descriptor;
}

jboolean Parcel_hasFileDescriptors(jlong ptr) {
    return P(ptr)->hasObjects(0, P(ptr)->size(), true);
}

jboolean Parcel_hasFileDescriptorsInRange(JNIEnv*, jclass, jlong ptr, jint offset, jint length) {
    return P(ptr)->hasObjects(static_cast<size_t>(offset), static_cast<size_t>(offset) + length, true);
}

jboolean Parcel_hasBinders(JNIEnv*, jclass, jlong ptr) {
    return P(ptr)->hasObjects(0, P(ptr)->size(), false);
}

jboolean Parcel_hasBindersInRange(JNIEnv*, jclass, jlong ptr, jint offset, jint length) {
    return P(ptr)->hasObjects(static_cast<size_t>(offset), static_cast<size_t>(offset) + length, false);
}

// --- Whole parcels -------------------------------------------------------------------

jbyteArray Parcel_marshall(JNIEnv* env, jclass, jlong ptr) {
    Parcel* parcel = P(ptr);
    if (parcel->hasAnyObjects()) {
        jniThrowException(env, "java/lang/RuntimeException",
                          "Tried to marshall a Parcel that contains objects (binders or FDs).");
        return nullptr;
    }
    jbyteArray array = env->NewByteArray(static_cast<jsize>(parcel->size()));
    if (array != nullptr && parcel->size() > 0) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(parcel->size()),
                                reinterpret_cast<const jbyte*>(parcel->data()));
    }
    return array;
}

void Parcel_unmarshall(JNIEnv* env, jclass, jlong ptr, jbyteArray array, jint offset, jint length) {
    if (array == nullptr || length < 0) return;
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(array, offset, length, bytes.data());
    if (env->ExceptionCheck()) return;
    P(ptr)->setData(env, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

jint Parcel_compareData(JNIEnv*, jclass, jlong thisPtr, jlong otherPtr) {
    Parcel* a = P(thisPtr);
    Parcel* b = P(otherPtr);
    if (a->size() != b->size()) return a->size() < b->size() ? -1 : 1;
    return a->size() == 0 ? 0 : memcmp(a->data(), b->data(), a->size());
}

jboolean Parcel_compareDataInRange(JNIEnv* env, jclass, jlong thisPtr, jint thisOffset, jlong otherPtr,
                                   jint otherOffset, jint length) {
    Parcel* a = P(thisPtr);
    Parcel* b = P(otherPtr);
    if (thisOffset < 0 || otherOffset < 0 || length < 0
        || static_cast<size_t>(thisOffset) + length > a->size()
        || static_cast<size_t>(otherOffset) + length > b->size()) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "range outside the parcel");
        return JNI_FALSE;
    }
    return memcmp(a->data() + thisOffset, b->data() + otherOffset, static_cast<size_t>(length)) == 0;
}

void Parcel_appendFrom(JNIEnv* env, jclass, jlong thisPtr, jlong otherPtr, jint offset, jint length) {
    if (offset < 0 || length < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    jint status = P(thisPtr)->appendFrom(env, *P(otherPtr), static_cast<size_t>(offset),
                                         static_cast<size_t>(length));
    Parcel_signalExceptionForError(env, nullptr, status);
}

const JNINativeMethod kMethods[] = {
        {"nativeMarkSensitive", "(J)V", (void*)Parcel_markSensitive},
        {"nativeMarkForBinder", "(JLandroid/os/IBinder;)V", (void*)Parcel_markForBinder},
        {"nativeIsForRpc", "(J)Z", (void*)Parcel_isForRpc},
        {"nativeDataSize", "(J)I", (void*)Parcel_dataSize},
        {"nativeDataAvail", "(J)I", (void*)Parcel_dataAvail},
        {"nativeDataPosition", "(J)I", (void*)Parcel_dataPosition},
        {"nativeDataCapacity", "(J)I", (void*)Parcel_dataCapacity},
        {"nativeSetDataSize", "(JI)V", (void*)Parcel_setDataSize},
        {"nativeSetDataPosition", "(JI)V", (void*)Parcel_setDataPosition},
        {"nativeSetDataCapacity", "(JI)V", (void*)Parcel_setDataCapacity},
        {"nativePushAllowFds", "(JZ)Z", (void*)Parcel_pushAllowFds},
        {"nativeRestoreAllowFds", "(JZ)V", (void*)Parcel_restoreAllowFds},
        {"nativeWriteByteArray", "(J[BII)V", (void*)Parcel_writeByteArray},
        {"nativeWriteBlob", "(J[BII)V", (void*)Parcel_writeBlob},
        {"nativeWriteInt", "(JI)I", (void*)Parcel_writeInt},
        {"nativeWriteLong", "(JJ)I", (void*)Parcel_writeLong},
        {"nativeWriteFloat", "(JF)I", (void*)Parcel_writeFloat},
        {"nativeWriteDouble", "(JD)I", (void*)Parcel_writeDouble},
        {"nativeSignalExceptionForError", "(I)V", (void*)Parcel_signalExceptionForError},
        {"nativeWriteString8", "(JLjava/lang/String;)V", (void*)Parcel_writeString8},
        {"nativeWriteString16", "(JLjava/lang/String;)V", (void*)Parcel_writeString16},
        {"nativeWriteStrongBinder", "(JLandroid/os/IBinder;)V", (void*)Parcel_writeStrongBinder},
        {"nativeWriteFileDescriptor", "(JLjava/io/FileDescriptor;)V", (void*)Parcel_writeFileDescriptor},
        {"nativeCreateByteArray", "(J)[B", (void*)Parcel_createByteArray},
        {"nativeReadByteArray", "(J[BI)Z", (void*)Parcel_readByteArray},
        {"nativeReadBlob", "(J)[B", (void*)Parcel_readBlob},
        {"nativeReadInt", "(J)I", (void*)Parcel_readInt},
        {"nativeReadLong", "(J)J", (void*)Parcel_readLong},
        {"nativeReadFloat", "(J)F", (void*)Parcel_readFloat},
        {"nativeReadDouble", "(J)D", (void*)Parcel_readDouble},
        {"nativeReadString8", "(J)Ljava/lang/String;", (void*)Parcel_readString8},
        {"nativeReadString16", "(J)Ljava/lang/String;", (void*)Parcel_readString16},
        {"nativeReadStrongBinder", "(J)Landroid/os/IBinder;", (void*)Parcel_readStrongBinder},
        {"nativeReadFileDescriptor", "(J)Ljava/io/FileDescriptor;", (void*)Parcel_readFileDescriptor},
        {"nativeCreate", "()J", (void*)Parcel_create},
        {"nativeFreeBuffer", "(J)V", (void*)Parcel_freeBuffer},
        {"nativeDestroy", "(J)V", (void*)Parcel_destroy},
        {"nativeMarshall", "(J)[B", (void*)Parcel_marshall},
        {"nativeUnmarshall", "(J[BII)V", (void*)Parcel_unmarshall},
        {"nativeCompareData", "(JJ)I", (void*)Parcel_compareData},
        {"nativeCompareDataInRange", "(JIJII)Z", (void*)Parcel_compareDataInRange},
        {"nativeAppendFrom", "(JJII)V", (void*)Parcel_appendFrom},
        {"nativeHasFileDescriptors", "(J)Z", (void*)Parcel_hasFileDescriptors},
        {"nativeHasFileDescriptorsInRange", "(JII)Z", (void*)Parcel_hasFileDescriptorsInRange},
        {"nativeHasBinders", "(J)Z", (void*)Parcel_hasBinders},
        {"nativeHasBindersInRange", "(JII)Z", (void*)Parcel_hasBindersInRange},
        {"nativeWriteInterfaceToken", "(JLjava/lang/String;)V", (void*)Parcel_writeInterfaceToken},
        {"nativeEnforceInterface", "(JLjava/lang/String;)V", (void*)Parcel_enforceInterface},
        {"nativeReplaceCallingWorkSourceUid", "(JI)Z", (void*)Parcel_replaceCallingWorkSourceUid},
        {"nativeReadCallingWorkSourceUid", "(J)I", (void*)Parcel_readCallingWorkSourceUid},
        {"nativeGetOpenAshmemSize", "(J)J", (void*)Parcel_getOpenAshmemSize},
        {"getGlobalAllocSize", "()J", (void*)Parcel_getGlobalAllocSize},
        {"getGlobalAllocCount", "()J", (void*)Parcel_getGlobalAllocCount},
};

}  // namespace

int register_android_os_Parcel(JNIEnv* env) {
    jclass descriptor = env->FindClass("java/io/FileDescriptor");
    if (descriptor == nullptr || env->GetJavaVM(&gVM) != JNI_OK) return -1;
    gFileDescriptorClass = static_cast<jclass>(env->NewGlobalRef(descriptor));
    gFileDescriptorInit = env->GetMethodID(descriptor, "<init>", "()V");
    gFileDescriptorGetInt = env->GetMethodID(descriptor, "getInt$", "()I");
    gFileDescriptorSetInt = env->GetMethodID(descriptor, "setInt$", "(I)V");
    if (gFileDescriptorInit == nullptr || gFileDescriptorGetInt == nullptr || gFileDescriptorSetInt == nullptr) {
        return -1;
    }
    return jniRegisterNativeMethods(env, "android/os/Parcel", kMethods, NELEM(kMethods));
}

}  // namespace android
