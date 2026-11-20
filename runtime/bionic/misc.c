// The rest of the C library for Android code: time, system information, dynamic loading,
// locales and multibyte conversion, and the functions that end the process.

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <xlocale.h>

#include "bionic_internal.h"

extern int* bionic___errno(void);

// --- Ending the process ------------------------------------------------------------------

static void fatal(const char* message) {
    a2i_android_log(7 /* ANDROID_LOG_FATAL */, "libc", message);
    abort();
}

// A function the app's library imports that this library does not have. The packer lists
// such imports when it converts the library.
BIONIC_EXPORT void bionic_unimplemented(void) {
    fatal("an Android C library function that the runtime does not provide was called");
}

BIONIC_EXPORT void bionic___stack_chk_fail(void) {
    fatal("stack corruption detected");
}
// Code built without thread slots for the guard reads this variable instead.
BIONIC_EXPORT uintptr_t bionic___stack_chk_guard = 0x5a3c91e704b8d200;

BIONIC_EXPORT void bionic___assert2(const char* file, int line, const char* function, const char* expression) {
    char message[512];
    snprintf(message, sizeof message, "%s:%d: %s: assertion \"%s\" failed", file, line, function, expression);
    fatal(message);
}
BIONIC_EXPORT void bionic___assert(const char* file, int line, const char* expression) {
    bionic___assert2(file, line, "?", expression);
}
BIONIC_EXPORT void bionic_android_set_abort_message(const char* message) {
    if (message != NULL) a2i_android_log(7, "libc", message);
}
BIONIC_EXPORT void bionic___fortify_fatal(const char* message) { fatal(message); }

// Thread-local destructors of C++ objects.
extern void _tlv_atexit(void (*destructor)(void*), void* object);
BIONIC_EXPORT int bionic___cxa_thread_atexit_impl(void (*destructor)(void*), void* object, void* dso) {
    _tlv_atexit(destructor, object);
    return 0;
}

// An iOS app cannot create processes.
BIONIC_EXPORT int bionic_fork(void) { bionic_set_errno(38 /* ENOSYS */); return -1; }
BIONIC_EXPORT int bionic_vfork(void) { bionic_set_errno(38); return -1; }
BIONIC_EXPORT int bionic_execvp(const char* file, char* const arguments[]) { bionic_set_errno(13 /* EACCES */); return -1; }
BIONIC_EXPORT int bionic_execv(const char* file, char* const arguments[]) { bionic_set_errno(13); return -1; }
BIONIC_EXPORT int bionic_execve(const char* file, char* const arguments[], char* const environment[]) {
    bionic_set_errno(13);
    return -1;
}
BIONIC_EXPORT int bionic_v_execl(const char* file, const char* argument, bionic_va_list* ap) { bionic_set_errno(13); return -1; }
BIONIC_EXPORT int bionic_v_execlp(const char* file, const char* argument, bionic_va_list* ap) { bionic_set_errno(13); return -1; }
BIONIC_EXPORT int bionic_system(const char* command) { return -1; }
BIONIC_EXPORT int bionic_waitpid(int pid, int* status, int options) { bionic_set_errno(10 /* ECHILD */); return -1; }
BIONIC_EXPORT void* bionic_popen(const char* command, const char* mode) { bionic_set_errno(38); return NULL; }

// --- Time --------------------------------------------------------------------------------

static clockid_t clock_to_host(int clock) {
    switch (clock) {
        case 0: case 5: case 8: case 11: return CLOCK_REALTIME;   // REALTIME, _COARSE, _ALARM, TAI
        case 1: case 6: return CLOCK_MONOTONIC;                   // MONOTONIC, _COARSE
        case 2: return CLOCK_PROCESS_CPUTIME_ID;
        case 3: return CLOCK_THREAD_CPUTIME_ID;
        case 4: return CLOCK_MONOTONIC_RAW;
        default: return CLOCK_MONOTONIC;                          // BOOTTIME: counts while asleep too
    }
}
BIONIC_EXPORT int bionic_clock_gettime(int clock, struct timespec* out) { return clock_gettime(clock_to_host(clock), out); }
BIONIC_EXPORT int bionic_clock_getres(int clock, struct timespec* out) { return clock_getres(clock_to_host(clock), out); }
BIONIC_EXPORT int bionic_clock_nanosleep(int clock, int flags, const struct timespec* request, struct timespec* remaining) {
    if (flags & 1 /* TIMER_ABSTIME */) {
        struct timespec now, delta;
        clock_gettime(clock_to_host(clock), &now);
        delta.tv_sec = request->tv_sec - now.tv_sec;
        delta.tv_nsec = request->tv_nsec - now.tv_nsec;
        if (delta.tv_nsec < 0) {
            delta.tv_sec--;
            delta.tv_nsec += 1000000000;
        }
        if (delta.tv_sec < 0) return 0;
        return nanosleep(&delta, NULL) == 0 ? 0 : bionic_errno_from_host(errno);
    }
    return nanosleep(request, remaining) == 0 ? 0 : bionic_errno_from_host(errno);
}

// --- System information ------------------------------------------------------------------

BIONIC_EXPORT long bionic_sysconf(int name) {
    switch (name) {
        case 0x00: return sysconf(_SC_ARG_MAX);
        case 0x06: return sysconf(_SC_CLK_TCK);
        case 0x0a: return sysconf(_SC_NGROUPS_MAX);
        case 0x0b: return sysconf(_SC_OPEN_MAX);
        case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
        case 0x48: return sysconf(_SC_GETPW_R_SIZE_MAX);
        case 0x4c: return PTHREAD_STACK_MIN;
        case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
        case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
        case 0x62: case 0x63: {  // physical pages, and those available
            uint64_t memory = 0;
            size_t size = sizeof memory;
            sysctlbyname("hw.memsize", &memory, &size, NULL, 0);
            return (long)(memory / (uint64_t)sysconf(_SC_PAGESIZE) / (name == 0x63 ? 2 : 1));
        }
        default:
            bionic_set_errno(22 /* EINVAL */);
            return -1;
    }
}

// What the kernel tells a Linux program about the machine at startup.
BIONIC_EXPORT unsigned long bionic_getauxval(unsigned long type) {
    switch (type) {
        case 6: return (unsigned long)sysconf(_SC_PAGESIZE);  // AT_PAGESZ
        case 16:  // AT_HWCAP: FP, ASIMD, AES, PMULL, SHA1, SHA2, CRC32, ATOMICS, which every iPhone CPU here has
            return 1u << 0 | 1u << 1 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8;
        default: return 0;
    }
}

struct linux_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
BIONIC_EXPORT int bionic_uname(struct linux_utsname* out) {
    memset(out, 0, sizeof *out);
    strcpy(out->sysname, "Linux");
    strcpy(out->nodename, "localhost");
    strcpy(out->release, "6.6.0");
    strcpy(out->version, "#1 SMP PREEMPT");
    strcpy(out->machine, "aarch64");
    strcpy(out->domainname, "localdomain");
    return 0;
}

// Android's system properties, as far as native code commonly asks.
BIONIC_EXPORT int bionic___system_property_get(const char* name, char* value) {
    static const char* const kProperties[][2] = {
            {"ro.build.version.sdk", "35"},
            {"ro.build.version.release", "15"},
            {"ro.product.cpu.abi", "arm64-v8a"},
            {"ro.product.manufacturer", "Apple"},
            {"ro.product.model", "iPhone"},
            {"ro.debuggable", "0"},
    };
    for (size_t i = 0; i < sizeof kProperties / sizeof kProperties[0]; i++) {
        if (strcmp(name, kProperties[i][0]) == 0) {
            strcpy(value, kProperties[i][1]);
            return (int)strlen(value);
        }
    }
    value[0] = 0;
    return 0;
}
BIONIC_EXPORT int bionic_android_get_device_api_level(void) { return 35; }

BIONIC_EXPORT ssize_t bionic_getrandom(void* buffer, size_t length, unsigned flags) {
    arc4random_buf(buffer, length);
    return (ssize_t)length;
}
BIONIC_EXPORT int bionic_getentropy(void* buffer, size_t length) {
    arc4random_buf(buffer, length);
    return 0;
}
BIONIC_EXPORT int bionic_prctl(int option, ...) { return 0; }

// Raw system calls: only the few that code makes because old C libraries lacked a
// wrapper.
BIONIC_EXPORT long bionic_v_syscall(long number, bionic_va_list* ap) {
    extern int bionic_gettid(void);
    switch (number) {
        case 178: return bionic_gettid();  // gettid
        case 172: return getpid();         // getpid
        case 278: {                        // getrandom
            void* buffer = (void*)bionic_va_gp(ap);
            size_t length = (size_t)bionic_va_gp(ap);
            arc4random_buf(buffer, length);
            return (long)length;
        }
        default:
            bionic_set_errno(38 /* ENOSYS */);
            return -1;
    }
}

BIONIC_EXPORT size_t bionic_malloc_usable_size(const void* pointer) { return malloc_size(pointer); }
BIONIC_EXPORT void* bionic_memalign(size_t alignment, size_t size) {
    void* pointer = NULL;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return posix_memalign(&pointer, alignment, size) == 0 ? pointer : NULL;
}
BIONIC_EXPORT void* bionic_aligned_alloc(size_t alignment, size_t size) { return bionic_memalign(alignment, size); }

BIONIC_EXPORT void bionic_sincos(double x, double* sine, double* cosine) {
    *sine = sin(x);
    *cosine = cos(x);
}
BIONIC_EXPORT void bionic_sincosf(float x, float* sine, float* cosine) {
    *sine = sinf(x);
    *cosine = cosf(x);
}

// Error messages take Android's error numbers.
BIONIC_EXPORT char* bionic_strerror(int error) { return strerror(bionic_errno_to_host(error)); }
// (The GNU form, which Android gives native code by default: returns the message.)
BIONIC_EXPORT char* bionic_strerror_r(int error, char* buffer, size_t size) {
    strerror_r(bionic_errno_to_host(error), buffer, size);
    return buffer;
}
BIONIC_EXPORT int bionic___xpg_strerror_r(int error, char* buffer, size_t size) {
    return strerror_r(bionic_errno_to_host(error), buffer, size);
}

BIONIC_EXPORT const char* bionic_inet_ntop(int family, const void* source, char* out, socklen_t size) {
    return inet_ntop(family == 10 /* Linux's AF_INET6 */ ? AF_INET6 : family, source, out, size);
}
BIONIC_EXPORT int bionic_inet_pton(int family, const char* source, void* out) {
    return inet_pton(family == 10 ? AF_INET6 : family, source, out);
}

// --- Fortified string functions: the plain function, with the buffer's size trusted --------

BIONIC_EXPORT void* bionic___memcpy_chk(void* d, const void* s, size_t n, size_t size) { return memcpy(d, s, n); }
BIONIC_EXPORT void* bionic___memmove_chk(void* d, const void* s, size_t n, size_t size) { return memmove(d, s, n); }
BIONIC_EXPORT void* bionic___memset_chk(void* d, int c, size_t n, size_t size) { return memset(d, c, n); }
BIONIC_EXPORT char* bionic___strcpy_chk(char* d, const char* s, size_t size) { return strcpy(d, s); }
BIONIC_EXPORT char* bionic___strcat_chk(char* d, const char* s, size_t size) { return strcat(d, s); }
BIONIC_EXPORT char* bionic___strncpy_chk(char* d, const char* s, size_t n, size_t size) { return strncpy(d, s, n); }
BIONIC_EXPORT char* bionic___strncpy_chk2(char* d, const char* s, size_t n, size_t ds, size_t ss) { return strncpy(d, s, n); }
BIONIC_EXPORT char* bionic___strncat_chk(char* d, const char* s, size_t n, size_t size) { return strncat(d, s, n); }
BIONIC_EXPORT size_t bionic___strlen_chk(const char* s, size_t size) { return strlen(s); }
BIONIC_EXPORT char* bionic___strchr_chk(const char* s, int c, size_t size) { return strchr(s, c); }
BIONIC_EXPORT char* bionic___strrchr_chk(const char* s, int c, size_t size) { return strrchr(s, c); }
BIONIC_EXPORT size_t bionic___strlcpy_chk(char* d, const char* s, size_t n, size_t size) { return strlcpy(d, s, n); }
BIONIC_EXPORT size_t bionic___strlcat_chk(char* d, const char* s, size_t n, size_t size) { return strlcat(d, s, n); }
BIONIC_EXPORT void* bionic___memchr_chk(const void* s, int c, size_t n, size_t size) { return memchr(s, c, n); }
BIONIC_EXPORT void bionic___FD_SET_chk(int fd, void* set, size_t size) { ((uint32_t*)set)[fd / 32] |= 1u << (fd % 32); }
BIONIC_EXPORT void bionic___FD_CLR_chk(int fd, void* set, size_t size) { ((uint32_t*)set)[fd / 32] &= ~(1u << (fd % 32)); }
BIONIC_EXPORT int bionic___FD_ISSET_chk(int fd, const void* set, size_t size) {
    return (((const uint32_t*)set)[fd / 32] >> (fd % 32)) & 1;
}

// --- Dynamic loading ---------------------------------------------------------------------

// Android's special handles and flag values.
static void* handle_to_host(void* handle) {
    if (handle == NULL) return RTLD_DEFAULT;
    if (handle == (void*)-1L) return RTLD_NEXT;
    return handle;
}
BIONIC_EXPORT void* bionic_dlsym(void* handle, const char* name) { return dlsym(handle_to_host(handle), name); }
BIONIC_EXPORT void* bionic_dlvsym(void* handle, const char* name, const char* version) {
    return dlsym(handle_to_host(handle), name);
}

// A library is asked for by its Android name ("libfoo.so"); what is in the app is the
// converted "libfoo.dylib" next to the runtime's libraries.
BIONIC_EXPORT void* bionic_dlopen(const char* name, int flags) {
    int host = (flags & 2 ? RTLD_NOW : RTLD_LAZY) | (flags & 0x100 ? RTLD_GLOBAL : RTLD_LOCAL);
    if (flags & 4) host |= RTLD_NOLOAD;
    if (name == NULL) return dlopen(NULL, host);
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t length = strlen(base);
    if (length > 3 && strcmp(base + length - 3, ".so") == 0) {
        char path[512];
        snprintf(path, sizeof path, "@rpath/%.*s.dylib", (int)(length - 3), base);
        void* handle = dlopen(path, host);
        if (handle != NULL) return handle;
    }
    return dlopen(name, host);
}

// The ELF program headers of each loaded Android library. Unwinders use them to find a
// library's exception tables. A converted library still contains its ELF header and
// program headers, one page after its Mach-O header.
struct linux_dl_phdr_info {
    uint64_t addr;
    const char* name;
    const void* phdr;
    uint16_t phnum;
    unsigned long long adds, subs;
    size_t tls_modid;
    void* tls_data;
};
BIONIC_EXPORT int bionic_dl_iterate_phdr(int (*callback)(struct linux_dl_phdr_info*, size_t, void*), void* data) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header_64* header = (const struct mach_header_64*)_dyld_get_image_header(i);
        unsigned long size = 0;
        if (header == NULL || getsegmentdata(header, "__A2I_SLOTS", &size) == NULL) continue;
        const unsigned char* elf = (const unsigned char*)header + 0x4000;
        if (memcmp(elf, "\x7f" "ELF", 4) != 0) continue;
        uint64_t phoff;
        uint16_t phnum;
        memcpy(&phoff, elf + 0x20, sizeof phoff);
        memcpy(&phnum, elf + 0x38, sizeof phnum);
        struct linux_dl_phdr_info info = {
                .addr = (uint64_t)(uintptr_t)elf,
                .name = _dyld_get_image_name(i),
                .phdr = elf + phoff,
                .phnum = phnum,
                .adds = count,
        };
        int result = callback(&info, sizeof info, data);
        if (result != 0) return result;
    }
    return 0;
}

// --- Locales and multibyte conversion ----------------------------------------------------

// Category numbers differ; Android's order is CTYPE, NUMERIC, TIME, COLLATE, MONETARY,
// MESSAGES, ALL.
BIONIC_EXPORT char* bionic_setlocale(int category, const char* locale) {
    static const int kCategories[] = {LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE, LC_MONETARY, LC_MESSAGES, LC_ALL};
    if (category < 0 || category > 6) return NULL;
    return setlocale(kCategories[category], locale);
}
BIONIC_EXPORT locale_t bionic_newlocale(int mask, const char* locale, locale_t base) {
    // bionic only has the "C" locale (with or without UTF-8), so every category is set alike.
    return newlocale(LC_ALL_MASK, locale, base);
}
BIONIC_EXPORT locale_t bionic_duplocale(locale_t locale) { return duplocale(locale); }
BIONIC_EXPORT size_t bionic___ctype_get_mb_cur_max(void) { return (size_t)MB_CUR_MAX; }

// Android's `long double` is a 128-bit float, returned in a vector register; these return
// a double there, which is exact for what the parsed text can hold in a double.
BIONIC_EXPORT double bionic_strtold(const char* s, char** end) { return strtod(s, end); }
BIONIC_EXPORT double bionic_strtold_l(const char* s, char** end, locale_t locale) { return strtod_l(s, end, locale); }
BIONIC_EXPORT double bionic_wcstold(const wchar_t* s, wchar_t** end) { return wcstod(s, end); }

// bionic's mbstate_t is four bytes; iOS's is 128. The caller's state cannot hold iOS's,
// so conversions keep their state in a per-thread one of ours. That is exact for UTF-8
// as long as a thread does not interleave two partial conversions.
static __thread mbstate_t tConversion;
BIONIC_EXPORT size_t bionic_mbrtowc(wchar_t* out, const char* s, size_t n, void* state) {
    return mbrtowc(out, s, n, &tConversion);
}
BIONIC_EXPORT size_t bionic_mbrlen(const char* s, size_t n, void* state) { return mbrlen(s, n, &tConversion); }
BIONIC_EXPORT size_t bionic_wcrtomb(char* out, wchar_t c, void* state) { return wcrtomb(out, c, &tConversion); }
BIONIC_EXPORT size_t bionic_mbsrtowcs(wchar_t* out, const char** source, size_t n, void* state) {
    return mbsrtowcs(out, source, n, &tConversion);
}
BIONIC_EXPORT size_t bionic_mbsnrtowcs(wchar_t* out, const char** source, size_t bytes, size_t n, void* state) {
    return mbsnrtowcs(out, source, bytes, n, &tConversion);
}
BIONIC_EXPORT size_t bionic_wcsrtombs(char* out, const wchar_t** source, size_t n, void* state) {
    return wcsrtombs(out, source, n, &tConversion);
}
BIONIC_EXPORT size_t bionic_wcsnrtombs(char* out, const wchar_t** source, size_t chars, size_t n, void* state) {
    return wcsnrtombs(out, source, chars, n, &tConversion);
}
BIONIC_EXPORT int bionic_mbtowc(wchar_t* out, const char* s, size_t n) { return mbtowc(out, s, n); }
BIONIC_EXPORT int bionic_wctomb(char* out, wchar_t c) { return wctomb(out, c); }
BIONIC_EXPORT size_t bionic_mbstowcs(wchar_t* out, const char* s, size_t n) { return mbstowcs(out, s, n); }
BIONIC_EXPORT size_t bionic_wcstombs(char* out, const wchar_t* s, size_t n) { return wcstombs(out, s, n); }
BIONIC_EXPORT int bionic_mbsinit(const void* state) { return 1; }
