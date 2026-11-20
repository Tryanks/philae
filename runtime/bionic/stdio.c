// printf, scanf and the rest of stdio for Android code.

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "bionic_internal.h"

// --- Streams ---------------------------------------------------------------------------

// Android's FILE is 152 bytes, and code built against old NDKs refers to the standard
// streams as elements of the array __sF. Nothing here is a real FILE: addresses inside
// the array stand for the three streams.
BIONIC_EXPORT char bionic___sF[3 * 152];
// Newer code uses these variables instead.
BIONIC_EXPORT FILE* bionic_stdin;
BIONIC_EXPORT FILE* bionic_stdout;
BIONIC_EXPORT FILE* bionic_stderr;

__attribute__((constructor)) static void init_streams(void) {
    bionic_stdin = stdin;
    bionic_stdout = stdout;
    bionic_stderr = stderr;
}

FILE* bionic_file(FILE* file) {
    char* p = (char*)file;
    if (p >= bionic___sF && p < bionic___sF + sizeof bionic___sF) {
        switch ((p - bionic___sF) / 152) {
            case 0: return stdin;
            case 1: return stdout;
            default: return stderr;
        }
    }
    return file;
}

#define F(file) bionic_file(file)
BIONIC_EXPORT int bionic_fclose(FILE* f) { return fclose(F(f)); }
BIONIC_EXPORT int bionic_feof(FILE* f) { return feof(F(f)); }
BIONIC_EXPORT int bionic_ferror(FILE* f) { return ferror(F(f)); }
BIONIC_EXPORT void bionic_clearerr(FILE* f) { clearerr(F(f)); }
BIONIC_EXPORT int bionic_fflush(FILE* f) { return fflush(f ? F(f) : NULL); }
BIONIC_EXPORT int bionic_fileno(FILE* f) { return fileno(F(f)); }
BIONIC_EXPORT int bionic_fgetc(FILE* f) { return fgetc(F(f)); }
BIONIC_EXPORT int bionic_getc(FILE* f) { return getc(F(f)); }
BIONIC_EXPORT char* bionic_fgets(char* s, int n, FILE* f) { return fgets(s, n, F(f)); }
BIONIC_EXPORT int bionic_fputc(int c, FILE* f) { return fputc(c, F(f)); }
BIONIC_EXPORT int bionic_putc(int c, FILE* f) { return putc(c, F(f)); }
BIONIC_EXPORT int bionic_fputs(const char* s, FILE* f) { return fputs(s, F(f)); }
BIONIC_EXPORT int bionic_ungetc(int c, FILE* f) { return ungetc(c, F(f)); }
BIONIC_EXPORT size_t bionic_fread(void* p, size_t size, size_t n, FILE* f) { return fread(p, size, n, F(f)); }
BIONIC_EXPORT size_t bionic_fwrite(const void* p, size_t size, size_t n, FILE* f) { return fwrite(p, size, n, F(f)); }
BIONIC_EXPORT int bionic_fseek(FILE* f, long offset, int whence) { return fseek(F(f), offset, whence); }
BIONIC_EXPORT int bionic_fseeko(FILE* f, off_t offset, int whence) { return fseeko(F(f), offset, whence); }
BIONIC_EXPORT long bionic_ftell(FILE* f) { return ftell(F(f)); }
BIONIC_EXPORT off_t bionic_ftello(FILE* f) { return ftello(F(f)); }
BIONIC_EXPORT void bionic_rewind(FILE* f) { rewind(F(f)); }
BIONIC_EXPORT void bionic_setbuf(FILE* f, char* buffer) { setbuf(F(f), buffer); }
BIONIC_EXPORT int bionic_setvbuf(FILE* f, char* buffer, int mode, size_t size) { return setvbuf(F(f), buffer, mode, size); }
BIONIC_EXPORT FILE* bionic_fopen(const char* path, const char* mode) { return fopen(path, mode); }
BIONIC_EXPORT FILE* bionic_fdopen(int fd, const char* mode) { return fdopen(fd, mode); }
BIONIC_EXPORT FILE* bionic_freopen(const char* path, const char* mode, FILE* f) { return freopen(path, mode, F(f)); }
BIONIC_EXPORT wint_t bionic_getwc(FILE* f) { return getwc(F(f)); }
BIONIC_EXPORT wint_t bionic_ungetwc(wint_t c, FILE* f) { return ungetwc(c, F(f)); }
BIONIC_EXPORT wint_t bionic_fputwc(wchar_t c, FILE* f) { return fputwc(c, F(f)); }
BIONIC_EXPORT int bionic_putchar(int c) { return putchar(c); }
BIONIC_EXPORT int bionic_puts(const char* s) { return puts(s); }
BIONIC_EXPORT void bionic_perror(const char* s) {
    extern int* bionic___errno(void);
    fprintf(stderr, "%s%s%s\n", s ? s : "", s && *s ? ": " : "", strerror(bionic_errno_to_host(*bionic___errno())));
}

// --- Formatted output ------------------------------------------------------------------

double bionic_va_long_double(bionic_va_list* ap) {
    uint64_t low, high;
    if (ap->vr_offs < 0) {
        uint64_t* p = (uint64_t*)((char*)ap->vr_top + ap->vr_offs);
        ap->vr_offs += 16;
        low = p[0];
        high = p[1];
    } else {
        uint64_t* p = (uint64_t*)(((uintptr_t)ap->stack + 15) & ~(uintptr_t)15);
        ap->stack = p + 2;
        low = p[0];
        high = p[1];
    }
    // IEEE binary128: sign, 15 bits of exponent, 112 of fraction.
    int exponent = (int)(high >> 48 & 0x7fff);
    uint64_t fraction = (high & 0xffffffffffffULL) << 4 | low >> 60;  // the top 52 bits
    double value;
    if (exponent == 0x7fff) {
        value = (fraction != 0 || (low << 4) != 0) ? NAN : INFINITY;
    } else if (exponent == 0) {
        value = 0;  // zero or below the range of double
    } else {
        value = ldexp(1.0 + (double)fraction / 4503599627370496.0, exponent - 16383);
    }
    return high >> 63 ? -value : value;
}

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
    int failed;
} buffer;

static void append(buffer* out, const char* bytes, size_t n) {
    if (out->length + n + 1 > out->capacity) {
        size_t capacity = (out->length + n + 1) * 2 + 64;
        char* data = realloc(out->data, capacity);
        if (data == NULL) {
            out->failed = 1;
            return;
        }
        out->data = data;
        out->capacity = capacity;
    }
    memcpy(out->data + out->length, bytes, n);
    out->length += n;
    out->data[out->length] = 0;
}

// Formats one directive with the host's printf. `spec` is the directive with any `*`
// already replaced by numbers.
#define EMIT(out, spec, value)                                \
    do {                                                      \
        char small[128];                                      \
        int n = snprintf(small, sizeof small, spec, value);   \
        if (n < (int)sizeof small) {                          \
            if (n > 0) append(out, small, (size_t)n);         \
        } else {                                              \
            char* big = malloc((size_t)n + 1);                \
            if (big != NULL) {                                \
                snprintf(big, (size_t)n + 1, spec, value);    \
                append(out, big, (size_t)n);                  \
                free(big);                                    \
            }                                                 \
        }                                                     \
    } while (0)

// The arguments are fetched the Android way, one directive at a time, and each directive
// is formatted by the host's printf, which knows every conversion and flag.
int bionic_format(char** result, const char* format, bionic_va_list* ap) {
    buffer out = {0};
    append(&out, "", 0);
    const char* p = format;
    while (*p) {
        const char* percent = strchr(p, '%');
        if (percent == NULL) {
            append(&out, p, strlen(p));
            break;
        }
        append(&out, p, (size_t)(percent - p));
        p = percent + 1;
        if (*p == '%') {
            append(&out, "%", 1);
            p++;
            continue;
        }
        // Rebuild the directive: flags, width, precision; the length modifier is replaced
        // by the one matching the type the value is passed to the host's printf as.
        char spec[64] = "%";
        size_t s = 1;
#define PUT(c) do { if (s < sizeof spec - 8) spec[s++] = (c); } while (0)
        while (*p && strchr("-+ #0'", *p)) PUT(*p++);
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (*p != '.') break;
                PUT(*p++);
            }
            if (*p == '*') {
                s += (size_t)snprintf(spec + s, 16, "%d", (int)bionic_va_gp(ap));
                p++;
            } else {
                while (*p >= '0' && *p <= '9') PUT(*p++);
            }
        }
        enum { kInt, kLong, kLongDouble } length = kInt;
        int wide = 0;
        for (;; p++) {
            if (*p == 'l') {
                length = kLong;
                wide = 1;
            } else if (*p == 'j' || *p == 'z' || *p == 't' || *p == 'q') {
                length = kLong;
            } else if (*p == 'L') {
                length = kLongDouble;
            } else if (*p != 'h') {
                break;
            }
        }
        char conversion = *p ? *p++ : 0;
        switch (conversion) {
            case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
                uint64_t value = bionic_va_gp(ap);
                if (length == kInt) {
                    // An int occupies the low half of its register; the rest is undefined.
                    value = (conversion == 'd' || conversion == 'i') ? (uint64_t)(int64_t)(int32_t)value
                                                                     : (uint32_t)value;
                }
                PUT('l');
                PUT('l');
                PUT(conversion);
                spec[s] = 0;
                EMIT(&out, spec, (unsigned long long)value);
                break;
            }
            case 'c':
                if (wide) PUT('l');
                PUT('c');
                spec[s] = 0;
                EMIT(&out, spec, (int)bionic_va_gp(ap));
                break;
            case 's':
                if (wide) PUT('l');
                PUT('s');
                spec[s] = 0;
                {
                    const void* string = (const void*)bionic_va_gp(ap);
                    if (string == NULL) {
                        append(&out, "(null)", 6);
                    } else {
                        EMIT(&out, spec, string);
                    }
                }
                break;
            case 'p':
                PUT('p');
                spec[s] = 0;
                EMIT(&out, spec, (void*)bionic_va_gp(ap));
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                double value = length == kLongDouble ? bionic_va_long_double(ap) : bionic_va_fp(ap);
                PUT(conversion);
                spec[s] = 0;
                EMIT(&out, spec, value);
                break;
            }
            case 'n': {
                void* target = (void*)bionic_va_gp(ap);
                if (length == kLong) {
                    *(long*)target = (long)out.length;
                } else {
                    *(int*)target = (int)out.length;
                }
                break;
            }
            case 'm': {  // a glibc and bionic extension: the message for errno
                extern int* bionic___errno(void);
                const char* message = strerror(bionic_errno_to_host(*bionic___errno()));
                append(&out, message, strlen(message));
                break;
            }
            default:  // not a directive after all
                append(&out, percent, (size_t)(p - percent));
                break;
        }
#undef PUT
    }
    if (out.failed || out.data == NULL) {
        free(out.data);
        *result = NULL;
        return -1;
    }
    *result = out.data;
    return (int)out.length;
}

BIONIC_EXPORT int bionic_v_snprintf(char* destination, size_t size, const char* format, bionic_va_list* ap) {
    char* text;
    int length = bionic_format(&text, format, ap);
    if (length < 0) return -1;
    if (size > 0) {
        size_t n = (size_t)length < size - 1 ? (size_t)length : size - 1;
        memcpy(destination, text, n);
        destination[n] = 0;
    }
    free(text);
    return length;
}

BIONIC_EXPORT int bionic_v_sprintf(char* destination, const char* format, bionic_va_list* ap) {
    char* text;
    int length = bionic_format(&text, format, ap);
    if (length < 0) return -1;
    memcpy(destination, text, (size_t)length + 1);
    free(text);
    return length;
}

BIONIC_EXPORT int bionic_v_asprintf(char** destination, const char* format, bionic_va_list* ap) {
    return bionic_format(destination, format, ap);
}

BIONIC_EXPORT int bionic_v_fprintf(FILE* file, const char* format, bionic_va_list* ap) {
    char* text;
    int length = bionic_format(&text, format, ap);
    if (length < 0) return -1;
    size_t written = fwrite(text, 1, (size_t)length, bionic_file(file));
    free(text);
    return written == (size_t)length ? length : -1;
}

BIONIC_EXPORT int bionic_v_printf(const char* format, bionic_va_list* ap) {
    return bionic_v_fprintf(stdout, format, ap);
}

BIONIC_EXPORT int bionic_v_dprintf(int fd, const char* format, bionic_va_list* ap) {
    char* text;
    int length = bionic_format(&text, format, ap);
    if (length < 0) return -1;
    ssize_t written = write(fd, text, (size_t)length);
    free(text);
    return (int)written;
}

// The "checked" variants that fortified Android builds call carry the destination's size.
BIONIC_EXPORT int bionic_v___sprintf_chk(char* destination, int flags, size_t destination_size,
                                         const char* format, bionic_va_list* ap) {
    return bionic_v_snprintf(destination, destination_size, format, ap);
}

BIONIC_EXPORT int bionic_v___snprintf_chk(char* destination, size_t size, int flags, size_t destination_size,
                                          const char* format, bionic_va_list* ap) {
    return bionic_v_snprintf(destination, size, format, ap);
}

BIONIC_EXPORT int bionic_v_swprintf(wchar_t* destination, size_t size, const wchar_t* format,
                                    bionic_va_list* ap) {
    // Formatted through the narrow engine; libc++ only uses this for numbers.
    size_t length = wcslen(format);
    char* narrow = malloc(length * 4 + 1);
    if (narrow == NULL || wcstombs(narrow, format, length * 4 + 1) == (size_t)-1) {
        free(narrow);
        return -1;
    }
    char* text;
    int n = bionic_format(&text, narrow, ap);
    free(narrow);
    if (n < 0) return -1;
    int result = -1;
    if ((size_t)n < size) {
        size_t converted = mbstowcs(destination, text, size);
        result = converted == (size_t)-1 ? -1 : (int)converted;
    }
    free(text);
    return result;
}

// The v-variants: Android passes a va_list by reference, so these are the same functions.
BIONIC_EXPORT int bionic_vsnprintf(char* d, size_t n, const char* f, bionic_va_list* ap) { return bionic_v_snprintf(d, n, f, ap); }
BIONIC_EXPORT int bionic_vsprintf(char* d, const char* f, bionic_va_list* ap) { return bionic_v_sprintf(d, f, ap); }
BIONIC_EXPORT int bionic_vasprintf(char** d, const char* f, bionic_va_list* ap) { return bionic_v_asprintf(d, f, ap); }
BIONIC_EXPORT int bionic_vfprintf(FILE* file, const char* f, bionic_va_list* ap) { return bionic_v_fprintf(file, f, ap); }
BIONIC_EXPORT int bionic_vprintf(const char* f, bionic_va_list* ap) { return bionic_v_printf(f, ap); }
BIONIC_EXPORT int bionic_vdprintf(int fd, const char* f, bionic_va_list* ap) { return bionic_v_dprintf(fd, f, ap); }
BIONIC_EXPORT int bionic_vswprintf(wchar_t* d, size_t n, const wchar_t* f, bionic_va_list* ap) { return bionic_v_swprintf(d, n, f, ap); }
BIONIC_EXPORT int bionic___vsnprintf_chk(char* d, size_t n, int flags, size_t ds, const char* f, bionic_va_list* ap) {
    return bionic_v_snprintf(d, n, f, ap);
}
BIONIC_EXPORT int bionic___vsprintf_chk(char* d, int flags, size_t ds, const char* f, bionic_va_list* ap) {
    return bionic_v_snprintf(d, ds, f, ap);
}

// --- Formatted input -------------------------------------------------------------------

// Every scanf argument is a pointer, so the arguments can be handed to the host's scanf
// as they are: iOS's va_list is simply a pointer to consecutive arguments.
#define SCAN_ARGUMENTS 32
static void collect(void* pointers[SCAN_ARGUMENTS], bionic_va_list* ap) {
    // (Reading more than were passed only copies unrelated stack words that are not used.)
    bionic_va_list copy = *ap;
    for (int i = 0; i < SCAN_ARGUMENTS; i++) pointers[i] = (void*)bionic_va_gp(&copy);
}

BIONIC_EXPORT int bionic_v_sscanf(const char* input, const char* format, bionic_va_list* ap) {
    void* pointers[SCAN_ARGUMENTS];
    collect(pointers, ap);
    return vsscanf(input, format, (va_list)pointers);
}

BIONIC_EXPORT int bionic_v_fscanf(FILE* file, const char* format, bionic_va_list* ap) {
    void* pointers[SCAN_ARGUMENTS];
    collect(pointers, ap);
    return vfscanf(bionic_file(file), format, (va_list)pointers);
}

BIONIC_EXPORT int bionic_v_scanf(const char* format, bionic_va_list* ap) {
    return bionic_v_fscanf(stdin, format, ap);
}

BIONIC_EXPORT int bionic_vsscanf(const char* s, const char* f, bionic_va_list* ap) { return bionic_v_sscanf(s, f, ap); }
BIONIC_EXPORT int bionic_vfscanf(FILE* file, const char* f, bionic_va_list* ap) { return bionic_v_fscanf(file, f, ap); }
