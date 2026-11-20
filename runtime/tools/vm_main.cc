// a2i-vm: command-line driver for the runtime, the equivalent of Android's `dalvikvm`.
// It exists so the runtime can be exercised without packaging an app, e.g. in the
// simulator:
//
//   xcrun simctl spawn booted a2i-vm <android-root> <data-dir> -cp app.jar com.example.Main arg...

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "android_vm.h"

static int Usage(const char* program) {
    fprintf(stderr,
            "usage: %s <android-root> <data-dir> [vm options] <main class> [arguments]\n"
            "  vm options are passed to the runtime, e.g. -cp <dex|jar|apk>, -Dname=value, -verbose:class\n",
            program);
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        return Usage(argv[0]);
    }

    // Everything up to the first non-option word configures the VM; that word is the main class.
    std::vector<const char*> vm_options;
    int i = 3;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-cp") == 0 || strcmp(argv[i], "-classpath") == 0) {
            if (i + 1 >= argc) return Usage(argv[0]);
            vm_options.push_back(argv[i++]);
        }
        vm_options.push_back(argv[i]);
    }
    if (i >= argc) {
        return Usage(argv[0]);
    }
    std::string main_class = argv[i++];
    for (char& c : main_class) {
        if (c == '.') c = '/';
    }

    a2i_vm_config config = {};
    config.android_root = argv[1];
    config.data_dir = argv[2];
    config.options = vm_options.data();
    config.option_count = vm_options.size();

    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    char error[512] = "";
    if (a2i_vm_create(&config, &vm, &env, error, sizeof error) != JNI_OK) {
        fprintf(stderr, "a2i-vm: %s\n", error);
        return 1;
    }

    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray arguments = env->NewObjectArray(argc - i, string_class, nullptr);
    for (int j = i; j < argc; j++) {
        env->SetObjectArrayElement(arguments, j - i, env->NewStringUTF(argv[j]));
    }

    jclass klass = env->FindClass(main_class.c_str());
    jmethodID entry = klass != nullptr
        ? env->GetStaticMethodID(klass, "main", "([Ljava/lang/String;)V")
        : nullptr;
    if (entry != nullptr) {
        env->CallStaticVoidMethod(klass, entry, arguments);
    }
    int status = 0;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        status = 1;
    }
    // As `java` does on return from main: wait for non-daemon threads, run shutdown hooks.
    vm->DetachCurrentThread();
    vm->DestroyJavaVM();
    return status;
}
