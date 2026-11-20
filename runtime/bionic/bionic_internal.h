// Shared by the parts of the bionic compatibility library.
//
// The library provides, for native code built for Android, the C library and NDK
// functions that code imports. A function `foo` of Android's is exported here as
// `bionic_foo` (the packer binds a converted library's imports accordingly) and behaves
// as Android's does: Android's argument conventions, constants, structure layouts and
// errno values, on top of the iOS functions that do the work.

#ifndef A2I_BIONIC_INTERNAL_H
#define A2I_BIONIC_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BIONIC_EXPORT __attribute__((visibility("default")))

// Android's va_list (the arm64 procedure call standard's). Functions that take one get
// it by reference.
typedef struct {
    void* stack;
    void* gr_top;
    void* vr_top;
    int gr_offs;
    int vr_offs;
} bionic_va_list;

// The next integer or pointer argument, and the next double.
static inline uint64_t bionic_va_gp(bionic_va_list* ap) {
    if (ap->gr_offs < 0) {
        uint64_t value = *(uint64_t*)((char*)ap->gr_top + ap->gr_offs);
        ap->gr_offs += 8;
        return value;
    }
    uint64_t value = *(uint64_t*)ap->stack;
    ap->stack = (char*)ap->stack + 8;
    return value;
}

static inline double bionic_va_fp(bionic_va_list* ap) {
    if (ap->vr_offs < 0) {
        double value = *(double*)((char*)ap->vr_top + ap->vr_offs);
        ap->vr_offs += 16;
        return value;
    }
    double value = *(double*)ap->stack;
    ap->stack = (char*)ap->stack + 8;
    return value;
}

// A `long double` argument, which on Android is a 128-bit float; returned as a double.
double bionic_va_long_double(bionic_va_list* ap);

// errno, in Android's (Linux's) numbering; see errno.c.
int bionic_errno_from_host(int host_errno);
int bionic_errno_to_host(int android_errno);
void bionic_set_errno(int android_errno);

// Android's stdin/stdout/stderr, old style: `&__sF[n]`. Maps those and passes real FILE
// pointers through.
FILE* bionic_file(FILE* file);

// Formats like printf with Android's va_list into a malloc'ed string; returns the length
// or -1.
int bionic_format(char** out, const char* format, bionic_va_list* ap);

void a2i_android_log(int priority, const char* tag, const char* message);

#endif
