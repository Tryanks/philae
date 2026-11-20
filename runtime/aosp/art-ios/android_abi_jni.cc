// See android_abi_jni.h.

#include "android_abi_jni.h"

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "art_method-inl.h"
#include "jni/jni_internal.h"
#include "scoped_thread_state_change-inl.h"

namespace art {

// --- Recognising Android code -------------------------------------------------------------

// A converted library carries a segment that nothing else has (its thread slot block),
// which is also the last part of its image. The address ranges of such libraries are
// noted as dyld loads them, so that the question, asked on every native method call, is
// answered by comparing addresses.
namespace {

struct Range {
  uintptr_t begin;
  uintptr_t end;
};
constexpr size_t kMaxAndroidLibraries = 256;
Range gAndroidLibraries[kMaxAndroidLibraries];
std::atomic<size_t> gAndroidLibraryCount{0};

void ImageAdded(const mach_header* header, intptr_t /* slide */) {
  unsigned long size = 0;
  uint8_t* slots = getsegmentdata(reinterpret_cast<const mach_header_64*>(header), "__A2I_SLOTS", &size);
  if (slots == nullptr) {
    return;
  }
  // (dyld calls this with its own lock held, one image at a time.)
  size_t count = gAndroidLibraryCount.load(std::memory_order_relaxed);
  if (count < kMaxAndroidLibraries) {
    gAndroidLibraries[count] = {reinterpret_cast<uintptr_t>(header),
                                reinterpret_cast<uintptr_t>(slots) + size};
    gAndroidLibraryCount.store(count + 1, std::memory_order_release);
  }
}

}  // namespace

bool IsAndroidAbiCode(const void* code) {
  static std::once_flag once;
  // Calls ImageAdded for every image already loaded, and from then on for each new one.
  std::call_once(once, [] { _dyld_register_func_for_add_image(&ImageAdded); });
  uintptr_t address = reinterpret_cast<uintptr_t>(code);
  size_t count = gAndroidLibraryCount.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; i++) {
    if (address >= gAndroidLibraries[i].begin && address < gAndroidLibraries[i].end) {
      return true;
    }
  }
  return false;
}

// --- The Android-facing JNIEnv and JavaVM ---------------------------------------------------

// What Android code holds as a JNIEnv*: like the real one, a pointer to a function table
// comes first. The assembly and the bionic library rely on `real` being the second word.
struct AndroidJniEnv {
  const JNINativeInterface* functions;
  JNIEnv* real;
};

struct AndroidJavaVm {
  const JNIInvokeInterface* functions;
  JavaVM* real;
};

// Android's va_list (AAPCS64); functions that take one receive a pointer to it.
struct AndroidVaList {
  void* stack;
  void* gr_top;
  void* vr_top;
  int gr_offs;
  int vr_offs;

  uint64_t NextGeneral() {
    if (gr_offs < 0) {
      uint64_t value = *reinterpret_cast<uint64_t*>(static_cast<char*>(gr_top) + gr_offs);
      gr_offs += 8;
      return value;
    }
    uint64_t value = *static_cast<uint64_t*>(stack);
    stack = static_cast<char*>(stack) + 8;
    return value;
  }

  double NextDouble() {
    if (vr_offs < 0) {
      double value = *reinterpret_cast<double*>(static_cast<char*>(vr_top) + vr_offs);
      vr_offs += 16;
      return value;
    }
    double value = *static_cast<double*>(stack);
    stack = static_cast<char*>(stack) + 8;
    return value;
  }
};

// The arguments of a method call, read from an Android va_list according to the method's
// signature, as the jvalue array that the `...A` JNI functions take.
class Arguments {
 public:
  Arguments(JNIEnv* env, jmethodID method, AndroidVaList* list) {
    uint32_t length = 0;
    const char* shorty;
    {
      ScopedObjectAccess soa(env);
      shorty = jni::DecodeArtMethod(method)
                   ->GetInterfaceMethodIfProxy(kRuntimePointerSize)
                   ->GetShorty(&length);
    }
    values_.resize(length > 0 ? length - 1 : 0);
    for (uint32_t i = 1; i < length; i++) {
      jvalue& value = values_[i - 1];
      switch (shorty[i]) {
        case 'Z': value.z = static_cast<jboolean>(list->NextGeneral()); break;
        case 'B': value.b = static_cast<jbyte>(list->NextGeneral()); break;
        case 'C': value.c = static_cast<jchar>(list->NextGeneral()); break;
        case 'S': value.s = static_cast<jshort>(list->NextGeneral()); break;
        case 'I': value.i = static_cast<jint>(list->NextGeneral()); break;
        case 'J': value.j = static_cast<jlong>(list->NextGeneral()); break;
        case 'F': value.f = static_cast<jfloat>(list->NextDouble()); break;  // promoted to double
        case 'D': value.d = list->NextDouble(); break;
        default: value.l = reinterpret_cast<jobject>(list->NextGeneral()); break;
      }
    }
  }

  const jvalue* data() const { return values_.data(); }

 private:
  std::vector<jvalue> values_;
};

}  // namespace art

// The `...V` functions, which the variadic assembly entries also end in.
extern "C" {

using art::AndroidJniEnv;
using art::AndroidVaList;
using art::Arguments;

jobject a2i_android_jni_NewObjectV(AndroidJniEnv* env, jclass clazz, jmethodID method, AndroidVaList* list) {
  Arguments arguments(env->real, method, list);
  return env->real->functions->NewObjectA(env->real, clazz, method, arguments.data());
}

#define A2I_CALL_FUNCTIONS(Type, type, return_keyword)                                             \
  type a2i_android_jni_Call##Type##MethodV(AndroidJniEnv* env, jobject object, jmethodID method,   \
                                           AndroidVaList* list) {                                  \
    Arguments arguments(env->real, method, list);                                                  \
    return_keyword env->real->functions->Call##Type##MethodA(env->real, object, method,            \
                                                             arguments.data());                    \
  }                                                                                                \
  type a2i_android_jni_CallNonvirtual##Type##MethodV(AndroidJniEnv* env, jobject object,           \
                                                     jclass clazz, jmethodID method,               \
                                                     AndroidVaList* list) {                        \
    Arguments arguments(env->real, method, list);                                                  \
    return_keyword env->real->functions->CallNonvirtual##Type##MethodA(env->real, object, clazz,   \
                                                                       method, arguments.data()); \
  }                                                                                                \
  type a2i_android_jni_CallStatic##Type##MethodV(AndroidJniEnv* env, jclass clazz,                 \
                                                 jmethodID method, AndroidVaList* list) {          \
    Arguments arguments(env->real, method, list);                                                  \
    return_keyword env->real->functions->CallStatic##Type##MethodA(env->real, clazz, method,       \
                                                                   arguments.data());              \
  }                                                                                                \
  type a2i_android_jni_Call##Type##Method(AndroidJniEnv*, jobject, jmethodID, ...);                \
  type a2i_android_jni_CallNonvirtual##Type##Method(AndroidJniEnv*, jobject, jclass, jmethodID, ...); \
  type a2i_android_jni_CallStatic##Type##Method(AndroidJniEnv*, jclass, jmethodID, ...);

A2I_CALL_FUNCTIONS(Object, jobject, return)
A2I_CALL_FUNCTIONS(Boolean, jboolean, return)
A2I_CALL_FUNCTIONS(Byte, jbyte, return)
A2I_CALL_FUNCTIONS(Char, jchar, return)
A2I_CALL_FUNCTIONS(Short, jshort, return)
A2I_CALL_FUNCTIONS(Int, jint, return)
A2I_CALL_FUNCTIONS(Long, jlong, return)
A2I_CALL_FUNCTIONS(Float, jfloat, return)
A2I_CALL_FUNCTIONS(Double, jdouble, return)
A2I_CALL_FUNCTIONS(Void, void, )
#undef A2I_CALL_FUNCTIONS

jobject a2i_android_jni_NewObject(AndroidJniEnv*, jclass, jmethodID, ...);

// 240 two-instruction forwarding entries; see the assembly.
extern const uint32_t a2i_android_jni_forwarders[];

}  // extern "C"

namespace art {

static AndroidJavaVm* gAndroidVm;

static jint GetJavaVM(AndroidJniEnv* env, JavaVM** vm) {
  JavaVM* real = nullptr;
  jint result = env->real->functions->GetJavaVM(env->real, &real);
  if (result == JNI_OK) {
    *vm = AndroidAbiJavaVm(real);
  }
  return result;
}

// The function table: every slot forwards, except those set individually below.
static const JNINativeInterface* AndroidJniFunctions() {
  static void* table[240];
  static std::once_flag once;
  std::call_once(once, [] {
    static_assert(sizeof(JNINativeInterface) <= sizeof(table), "JNI function table grew");
    for (size_t i = 0; i < 240; i++) {
      table[i] = const_cast<uint32_t*>(&a2i_android_jni_forwarders[i * 2]);
    }
#define A2I_SLOT(name) table[offsetof(JNINativeInterface, name) / sizeof(void*)]
#define A2I_OVERRIDE(name) A2I_SLOT(name) = reinterpret_cast<void*>(&a2i_android_jni_##name);
#define A2I_OVERRIDE_CALLS(Type)                \
    A2I_OVERRIDE(Call##Type##Method)             \
    A2I_OVERRIDE(Call##Type##MethodV)            \
    A2I_OVERRIDE(CallNonvirtual##Type##Method)   \
    A2I_OVERRIDE(CallNonvirtual##Type##MethodV)  \
    A2I_OVERRIDE(CallStatic##Type##Method)       \
    A2I_OVERRIDE(CallStatic##Type##MethodV)
    A2I_OVERRIDE(NewObject)
    A2I_OVERRIDE(NewObjectV)
    A2I_OVERRIDE_CALLS(Object)
    A2I_OVERRIDE_CALLS(Boolean)
    A2I_OVERRIDE_CALLS(Byte)
    A2I_OVERRIDE_CALLS(Char)
    A2I_OVERRIDE_CALLS(Short)
    A2I_OVERRIDE_CALLS(Int)
    A2I_OVERRIDE_CALLS(Long)
    A2I_OVERRIDE_CALLS(Float)
    A2I_OVERRIDE_CALLS(Double)
    A2I_OVERRIDE_CALLS(Void)
    A2I_SLOT(GetJavaVM) = reinterpret_cast<void*>(&GetJavaVM);
#undef A2I_OVERRIDE_CALLS
#undef A2I_OVERRIDE
#undef A2I_SLOT
  });
  return reinterpret_cast<const JNINativeInterface*>(table);
}

JNIEnv* AndroidAbiJniEnv(JNIEnv* real) {
  if (real == nullptr) {
    return nullptr;
  }
  // One per thread, like the real JNIEnv.
  static thread_local AndroidJniEnv env;
  env.functions = AndroidJniFunctions();
  env.real = real;
  return reinterpret_cast<JNIEnv*>(&env);
}

static jint DestroyJavaVM(AndroidJavaVm* vm) {
  return vm->real->functions->DestroyJavaVM(vm->real);
}

static jint AttachCurrentThread(AndroidJavaVm* vm, JNIEnv** env, void* arguments) {
  JNIEnv* real = nullptr;
  jint result = vm->real->functions->AttachCurrentThread(vm->real, &real, arguments);
  if (result == JNI_OK) {
    *env = AndroidAbiJniEnv(real);
  }
  return result;
}

static jint AttachCurrentThreadAsDaemon(AndroidJavaVm* vm, JNIEnv** env, void* arguments) {
  JNIEnv* real = nullptr;
  jint result = vm->real->functions->AttachCurrentThreadAsDaemon(vm->real, &real, arguments);
  if (result == JNI_OK) {
    *env = AndroidAbiJniEnv(real);
  }
  return result;
}

static jint DetachCurrentThread(AndroidJavaVm* vm) {
  return vm->real->functions->DetachCurrentThread(vm->real);
}

static jint GetEnv(AndroidJavaVm* vm, void** env, jint version) {
  void* real = nullptr;
  jint result = vm->real->functions->GetEnv(vm->real, &real, version);
  *env = result == JNI_OK ? AndroidAbiJniEnv(static_cast<JNIEnv*>(real)) : nullptr;
  return result;
}

JavaVM* AndroidAbiJavaVm(JavaVM* real) {
  static std::once_flag once;
  std::call_once(once, [real] {
    static JNIInvokeInterface functions = {};
    functions.DestroyJavaVM = reinterpret_cast<jint (*)(JavaVM*)>(&DestroyJavaVM);
    functions.AttachCurrentThread =
        reinterpret_cast<jint (*)(JavaVM*, JNIEnv**, void*)>(&AttachCurrentThread);
    functions.DetachCurrentThread = reinterpret_cast<jint (*)(JavaVM*)>(&DetachCurrentThread);
    functions.GetEnv = reinterpret_cast<jint (*)(JavaVM*, void**, jint)>(&GetEnv);
    functions.AttachCurrentThreadAsDaemon =
        reinterpret_cast<jint (*)(JavaVM*, JNIEnv**, void*)>(&AttachCurrentThreadAsDaemon);
    gAndroidVm = new AndroidJavaVm{&functions, real};
  });
  return reinterpret_cast<JavaVM*>(gAndroidVm);
}

}  // namespace art
