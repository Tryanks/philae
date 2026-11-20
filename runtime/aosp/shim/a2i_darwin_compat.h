// Force-included into every AOSP translation unit: Linux/bionic names that Darwin spells
// differently. Darwin's file offsets are always 64-bit, so the *64 variants are aliases.
#pragma once
// (Assembly files get the same -include, hence the __ASSEMBLER__ guard.)
#if defined(__APPLE__) && !defined(__ASSEMBLER__)
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

// (libutils' <utils/Compat.h> defines these as functions for Apple itself.)
#ifndef A2I_HAVE_UTILS_COMPAT
#define lseek64 lseek
#define pread64 pread
#define pwrite64 pwrite
#define ftruncate64 ftruncate
#endif

// Darwin has no real-time signals. libcore interrupts threads blocked in I/O with
// "SIGRTMAX - 2"; with this value that is SIGINFO (29), which nothing else on iOS uses and
// whose default action is to be ignored.
#define SIGRTMAX 31
#define __SIGRTMAX SIGRTMAX

// Only reached from the userfaultfd collector, which is never selected on Darwin.
#define MADV_REMOVE MADV_FREE

// Darwin cannot move mappings. ART's only unguarded callers (the zygote JIT) treat failure
// as "keep the old contents".
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
static inline void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...) {
    (void)old_address, (void)old_size, (void)new_size, (void)flags;
    errno = ENOSYS;
    return MAP_FAILED;
}
#endif
