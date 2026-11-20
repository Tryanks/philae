#include "android_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

// The boot class path, in the order Android's BOOTCLASSPATH lists these modules.
// Paths are relative to the Android root.
const char* const kBootJars[] = {
    "apex/com.android.art/javalib/core-oj.jar",
    "apex/com.android.art/javalib/core-libart.jar",
    "apex/com.android.art/javalib/okhttp.jar",
    "apex/com.android.art/javalib/bouncycastle.jar",
    "apex/com.android.art/javalib/apache-xml.jar",
    "apex/com.android.i18n/javalib/core-icu4j.jar",
    "apex/com.android.conscrypt/javalib/conscrypt.jar",
    // This project's own platform code (runtime/java). It precedes the framework so that
    // its replacements for individual framework classes are the ones that get loaded.
    "framework/a2i-boot.jar",
    // The Android framework (android.*, com.android.internal.*); scripts/build-framework.py.
    "framework/framework.jar",
};

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

jint a2i_vm_create(const a2i_vm_config* config, JavaVM** vm, JNIEnv** env,
                   char* error, size_t error_size) {
    const std::string root = config->android_root;
    const std::string data = config->data_dir;

    std::string boot_class_path;
    for (const char* jar : kBootJars) {
        std::string path = root + "/" + jar;
        if (!IsFile(path)) {
            snprintf(error, error_size, "boot class path entry is missing: %s", path.c_str());
            return JNI_ERR;
        }
        boot_class_path += (boot_class_path.empty() ? "" : ":") + path;
    }

    // ART locates its modules through these variables when it is not running on a device
    // with the fixed /system and /apex mount points.
    setenv("ANDROID_ROOT", root.c_str(), 1);
    setenv("ANDROID_ART_ROOT", (root + "/apex/com.android.art").c_str(), 1);
    setenv("ANDROID_I18N_ROOT", (root + "/apex/com.android.i18n").c_str(), 1);
    setenv("ANDROID_TZDATA_ROOT", (root + "/apex/com.android.tzdata").c_str(), 1);
    setenv("ANDROID_DATA", data.c_str(), 1);
    // Android's logging reads its threshold from here; default to info and above.
    setenv("ANDROID_LOG_TAGS", "*:i", 0);

    std::vector<std::string> options = {
        "-Xbootclasspath:" + boot_class_path,
        "-Xbootclasspath-locations:" + boot_class_path,
        // iOS only executes signed code pages, so nothing may be compiled at run time:
        // interpret everything and never start the JIT.
        "-Xint",
        "-Xusejit:false",
        // No precompiled boot image is shipped yet; classes are loaded straight from the jars.
        "-Xnoimage-dex2oat",
        "-Xnorelocate",
        "-Xmx256m",
    };
    for (size_t i = 0; i < config->option_count; i++) {
        options.push_back(config->options[i]);
    }

    std::vector<JavaVMOption> vm_options(options.size());
    for (size_t i = 0; i < options.size(); i++) {
        vm_options[i].optionString = options[i].c_str();
        vm_options[i].extraInfo = nullptr;
    }
    JavaVMInitArgs args;
    args.version = JNI_VERSION_1_6;
    args.nOptions = static_cast<jint>(vm_options.size());
    args.options = vm_options.data();
    args.ignoreUnrecognized = JNI_FALSE;

    jint result = JNI_CreateJavaVM(vm, env, &args);
    if (result != JNI_OK) {
        snprintf(error, error_size, "JNI_CreateJavaVM failed with %d (see log)", result);
    }
    return result;
}
