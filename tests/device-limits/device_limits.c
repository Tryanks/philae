// Makes a simulator process obey a real iPhone's rules about executable memory, to find
// code that would fail on a device without having one.
//
// A simulator process runs on the macOS kernel, which lets it create executable memory
// at will. iOS does not: an app may only execute pages that come from its signed
// binaries. This library is inserted into a process (DYLD_INSERT_LIBRARIES) and refuses,
// as iOS would, every request for executable memory made through mmap or mprotect,
// logging who asked. Loading signed libraries is unaffected: dyld maps those itself.
//
// It also logs large address space reservations, since a device process has less
// address space than a Mac's (how much less varies by device and iOS version).
//
// Build and use: see tests/device-limits/run.sh.

#include <errno.h>
#include <execinfo.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define INTERPOSE(replacement, original)                                              \
    __attribute__((used)) static const struct { const void *r, *o; } interpose_##original \
        __attribute__((section("__DATA,__interpose"))) = {(const void *)replacement, (const void *)original}

static void report(const char *what, void *addr, size_t length, int prot, int flags) {
    char line[256];
    int n = snprintf(line, sizeof line, "DEVICE-LIMIT: %s addr=%p len=%zu prot=%#x flags=%#x\n", what, addr,
                     length, prot, flags);
    write(STDERR_FILENO, line, (size_t)n);
    void *frames[24];
    int count = backtrace(frames, 24);
    backtrace_symbols_fd(frames + 2, count - 2, STDERR_FILENO);
}

static void *limited_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (prot & PROT_EXEC) {
        report(fd < 0 ? "DENIED mmap of anonymous executable memory" : "DENIED mmap of a file as executable",
               addr, length, prot, flags);
        errno = EPERM;
        return MAP_FAILED;
    }
    if (flags & MAP_JIT) {
        report("DENIED mmap with MAP_JIT", addr, length, prot, flags);
        errno = EPERM;
        return MAP_FAILED;
    }
    if (length >= ((size_t)1 << 30)) {
        report("note: large reservation", addr, length, prot, flags);
    }
    return mmap(addr, length, prot, flags, fd, offset);
}

static int limited_mprotect(void *addr, size_t length, int prot) {
    if (prot & PROT_EXEC) {
        report("DENIED mprotect to executable", addr, length, prot, 0);
        errno = EPERM;
        return -1;
    }
    return mprotect(addr, length, prot);
}

INTERPOSE(limited_mmap, mmap);
INTERPOSE(limited_mprotect, mprotect);
