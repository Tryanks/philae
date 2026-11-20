// Android's NDK libraries, for the app's native code: liblog and libjnigraphics.

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "bionic_internal.h"

// --- liblog ------------------------------------------------------------------------------

// Messages go to the runtime's Android log (the same one the framework writes to), which
// the runtime's own liblog exports.
void a2i_android_log(int priority, const char* tag, const char* message) {
    static int (*write_log)(int, const char*, const char*);
    static int looked_up;
    if (!looked_up) {
        write_log = dlsym(RTLD_DEFAULT, "__android_log_write");
        looked_up = 1;
    }
    if (write_log != NULL) {
        write_log(priority, tag ? tag : "", message);
    } else {
        static const char kLevels[] = "??VDIWEF";
        fprintf(stderr, "%c %s: %s\n", kLevels[priority >= 0 && priority <= 7 ? priority : 0], tag ? tag : "", message);
    }
}

BIONIC_EXPORT int bionic___android_log_write(int priority, const char* tag, const char* message) {
    a2i_android_log(priority, tag, message);
    return 1;
}

BIONIC_EXPORT int bionic_v___android_log_print(int priority, const char* tag, const char* format, bionic_va_list* ap) {
    char* message;
    if (bionic_format(&message, format, ap) < 0) return -1;
    a2i_android_log(priority, tag, message);
    free(message);
    return 1;
}
BIONIC_EXPORT int bionic___android_log_vprint(int priority, const char* tag, const char* format, bionic_va_list* ap) {
    return bionic_v___android_log_print(priority, tag, format, ap);
}
BIONIC_EXPORT int bionic_v___android_log_buf_print(int buffer, int priority, const char* tag, const char* format,
                                                   bionic_va_list* ap) {
    return bionic_v___android_log_print(priority, tag, format, ap);
}
BIONIC_EXPORT int bionic___android_log_buf_write(int buffer, int priority, const char* tag, const char* message) {
    a2i_android_log(priority, tag, message);
    return 1;
}
BIONIC_EXPORT void bionic_v___android_log_assert(const char* condition, const char* tag, const char* format,
                                                 bionic_va_list* ap) {
    char* message = NULL;
    if (format != NULL) bionic_format(&message, format, ap);
    a2i_android_log(7, tag, message ? message : condition ? condition : "assertion failed");
    abort();
}
BIONIC_EXPORT int bionic___android_log_is_loggable(int priority, const char* tag, int default_priority) {
    return priority >= default_priority;
}

// syslog, which Android routes to the same log.
BIONIC_EXPORT void bionic_openlog(const char* identity, int options, int facility) {}
BIONIC_EXPORT void bionic_closelog(void) {}
BIONIC_EXPORT void bionic_v_syslog(int priority, const char* format, bionic_va_list* ap) {
    char* message;
    if (bionic_format(&message, format, ap) < 0) return;
    // syslog's levels run from 0 (emergency) to 7 (debug); Android's from 2 (verbose) to 7 (fatal).
    static const int kPriorities[] = {7, 7, 7, 6, 5, 4, 4, 3};
    a2i_android_log(kPriorities[priority & 7], "syslog", message);
    free(message);
}

// --- libjnigraphics ----------------------------------------------------------------------

// Access to a Bitmap's pixels. The graphics library implements these for the JNIEnv the
// runtime's own code uses; Android-ABI code holds a different one that wraps it (its
// second word points to the real one; see the runtime's Android-ABI JNI layer).
static void* real_env(void* env) {
    return ((void**)env)[1];
}

#define GRAPHICS_FUNCTION(name)                                                            \
    static int (*function)();                                                              \
    if (function == NULL) function = (int (*)())dlsym(RTLD_DEFAULT, name);                 \
    if (function == NULL) return -1 /* ANDROID_BITMAP_RESULT_BAD_PARAMETER */

BIONIC_EXPORT int bionic_AndroidBitmap_getInfo(void* env, void* bitmap, void* info) {
    GRAPHICS_FUNCTION("AndroidBitmap_getInfo");
    return ((int (*)(void*, void*, void*))function)(real_env(env), bitmap, info);
}
BIONIC_EXPORT int bionic_AndroidBitmap_lockPixels(void* env, void* bitmap, void** pixels) {
    GRAPHICS_FUNCTION("AndroidBitmap_lockPixels");
    return ((int (*)(void*, void*, void**))function)(real_env(env), bitmap, pixels);
}
BIONIC_EXPORT int bionic_AndroidBitmap_unlockPixels(void* env, void* bitmap) {
    GRAPHICS_FUNCTION("AndroidBitmap_unlockPixels");
    return ((int (*)(void*, void*))function)(real_env(env), bitmap);
}
