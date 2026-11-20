// Creation of the Android runtime (ART) inside this process.
#pragma once
#include <stddef.h>

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Directory laid out like the root of an Android device: apex/com.android.art/javalib/*.jar,
    // apex/com.android.i18n/etc/icu/*.dat and so on.
    const char *android_root;
    // Writable directory that plays the role of Android's /data.
    const char *data_dir;
    // Extra runtime options (e.g. "-verbose:class"), may be NULL when option_count is 0.
    const char *const *options;
    size_t option_count;
} a2i_vm_config;

// Start the runtime. Returns JNI_OK and fills `vm`/`env` (attached to the calling thread),
// or a JNI error code with a description in `error`.
jint a2i_vm_create(const a2i_vm_config *config, JavaVM **vm, JNIEnv **env,
                   char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
