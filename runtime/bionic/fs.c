// Files, directories and memory mappings for Android code: the functions whose flags or
// structures differ between Linux and iOS.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "bionic_internal.h"

// --- open ------------------------------------------------------------------------------

// Linux (arm64) open flags.
enum {
    L_O_CREAT = 0100,
    L_O_EXCL = 0200,
    L_O_NOCTTY = 0400,
    L_O_TRUNC = 01000,
    L_O_APPEND = 02000,
    L_O_NONBLOCK = 04000,
    L_O_DSYNC = 010000,
    L_O_DIRECTORY = 040000,
    L_O_NOFOLLOW = 0100000,
    L_O_CLOEXEC = 02000000,
    L_O_SYNC = 04000000,
};

static int open_flags_to_host(int flags) {
    int host = flags & 3;  // O_RDONLY, O_WRONLY, O_RDWR
    if (flags & L_O_CREAT) host |= O_CREAT;
    if (flags & L_O_EXCL) host |= O_EXCL;
    if (flags & L_O_NOCTTY) host |= O_NOCTTY;
    if (flags & L_O_TRUNC) host |= O_TRUNC;
    if (flags & L_O_APPEND) host |= O_APPEND;
    if (flags & L_O_NONBLOCK) host |= O_NONBLOCK;
    if (flags & L_O_DSYNC) host |= O_DSYNC;
    if (flags & L_O_DIRECTORY) host |= O_DIRECTORY;
    if (flags & L_O_NOFOLLOW) host |= O_NOFOLLOW;
    if (flags & L_O_CLOEXEC) host |= O_CLOEXEC;
    if (flags & L_O_SYNC) host |= O_SYNC;
    return host;
}

static int open_flags_from_host(int host) {
    int flags = host & 3;
    if (host & O_APPEND) flags |= L_O_APPEND;
    if (host & O_NONBLOCK) flags |= L_O_NONBLOCK;
    if (host & O_SYNC) flags |= L_O_SYNC;
    return flags;
}

BIONIC_EXPORT int bionic_v_open(const char* path, int flags, bionic_va_list* ap) {
    mode_t mode = (flags & L_O_CREAT) ? (mode_t)bionic_va_gp(ap) : 0;
    return open(path, open_flags_to_host(flags), mode);
}

BIONIC_EXPORT int bionic_v_open64(const char* path, int flags, bionic_va_list* ap) {
    return bionic_v_open(path, flags, ap);
}

BIONIC_EXPORT int bionic_v_openat(int dir, const char* path, int flags, bionic_va_list* ap) {
    mode_t mode = (flags & L_O_CREAT) ? (mode_t)bionic_va_gp(ap) : 0;
    return openat(dir == -100 /* Linux's AT_FDCWD */ ? AT_FDCWD : dir, path, open_flags_to_host(flags), mode);
}

// The fortified open: no mode argument.
BIONIC_EXPORT int bionic___open_2(const char* path, int flags) {
    return open(path, open_flags_to_host(flags), 0);
}

BIONIC_EXPORT int bionic_creat(const char* path, mode_t mode) {
    return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

// --- fcntl and ioctl -------------------------------------------------------------------

struct linux_flock {
    short l_type;
    short l_whence;
    long l_start;
    long l_len;
    int l_pid;
};

BIONIC_EXPORT int bionic_v_fcntl(int fd, int command, bionic_va_list* ap) {
    uint64_t argument = bionic_va_gp(ap);
    switch (command) {
        case 0: return fcntl(fd, F_DUPFD, (int)argument);
        case 1: return fcntl(fd, F_GETFD);
        case 2: return fcntl(fd, F_SETFD, (int)argument);  // FD_CLOEXEC is 1 on both
        case 3: {
            int flags = fcntl(fd, F_GETFL);
            return flags < 0 ? flags : open_flags_from_host(flags);
        }
        case 4: return fcntl(fd, F_SETFL, open_flags_to_host((int)argument));
        case 1030: return fcntl(fd, F_DUPFD_CLOEXEC, (int)argument);
        case 5: case 6: case 7: {  // F_GETLK, F_SETLK, F_SETLKW
            struct linux_flock* lock = (struct linux_flock*)argument;
            struct flock host = {
                    .l_type = lock->l_type == 0 ? F_RDLCK : lock->l_type == 1 ? F_WRLCK : F_UNLCK,
                    .l_whence = lock->l_whence,
                    .l_start = lock->l_start,
                    .l_len = lock->l_len,
                    .l_pid = lock->l_pid,
            };
            int result = fcntl(fd, command == 5 ? F_GETLK : command == 6 ? F_SETLK : F_SETLKW, &host);
            if (result == 0 && command == 5) {
                lock->l_type = host.l_type == F_RDLCK ? 0 : host.l_type == F_WRLCK ? 1 : 2;
                lock->l_whence = host.l_whence;
                lock->l_start = host.l_start;
                lock->l_len = host.l_len;
                lock->l_pid = host.l_pid;
            }
            return result;
        }
        default:
            bionic_set_errno(22 /* EINVAL */);
            return -1;
    }
}

BIONIC_EXPORT int bionic_v_ioctl(int fd, int request, bionic_va_list* ap) {
    void* argument = (void*)bionic_va_gp(ap);
    switch ((unsigned)request) {
        case 0x541B: return ioctl(fd, FIONREAD, argument);
        case 0x5421: return ioctl(fd, FIONBIO, argument);
        case 0x5413: return ioctl(fd, TIOCGWINSZ, argument);  // struct winsize agrees
        default:
            bionic_set_errno(25 /* ENOTTY */);
            return -1;
    }
}

// --- stat ------------------------------------------------------------------------------

struct linux_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    unsigned long __pad1;
    long st_size;
    int st_blksize;
    int __pad2;
    long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned int __unused4;
    unsigned int __unused5;
};

static int stat_from_host(int result, const struct stat* host, struct linux_stat* out) {
    if (result != 0) return result;
    memset(out, 0, sizeof *out);
    out->st_dev = (unsigned long)host->st_dev;
    out->st_ino = host->st_ino;
    out->st_mode = host->st_mode;  // the file type and permission bits agree
    out->st_nlink = host->st_nlink;
    out->st_uid = host->st_uid;
    out->st_gid = host->st_gid;
    out->st_rdev = (unsigned long)host->st_rdev;
    out->st_size = host->st_size;
    out->st_blksize = host->st_blksize;
    out->st_blocks = host->st_blocks;
    out->st_atim = host->st_atimespec;
    out->st_mtim = host->st_mtimespec;
    out->st_ctim = host->st_ctimespec;
    return 0;
}

BIONIC_EXPORT int bionic_stat(const char* path, struct linux_stat* out) {
    struct stat host;
    return stat_from_host(stat(path, &host), &host, out);
}
BIONIC_EXPORT int bionic_lstat(const char* path, struct linux_stat* out) {
    struct stat host;
    return stat_from_host(lstat(path, &host), &host, out);
}
BIONIC_EXPORT int bionic_fstat(int fd, struct linux_stat* out) {
    struct stat host;
    return stat_from_host(fstat(fd, &host), &host, out);
}
BIONIC_EXPORT int bionic_fstatat(int dir, const char* path, struct linux_stat* out, int flags) {
    struct stat host;
    int host_flags = (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) ? AT_SYMLINK_NOFOLLOW : 0;
    return stat_from_host(fstatat(dir == -100 ? AT_FDCWD : dir, path, &host, host_flags), &host, out);
}
BIONIC_EXPORT int bionic_stat64(const char* path, struct linux_stat* out) { return bionic_stat(path, out); }
BIONIC_EXPORT int bionic_lstat64(const char* path, struct linux_stat* out) { return bionic_lstat(path, out); }
BIONIC_EXPORT int bionic_fstat64(int fd, struct linux_stat* out) { return bionic_fstat(fd, out); }

// --- Directories -----------------------------------------------------------------------

struct linux_dirent {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;  // the DT_* values agree
    char d_name[256];
};

static struct linux_dirent* dirent_from_host(const struct dirent* host, struct linux_dirent* out) {
    out->d_ino = host->d_ino;
    out->d_off = (int64_t)host->d_seekoff;
    out->d_reclen = sizeof *out;
    out->d_type = host->d_type;
    strlcpy(out->d_name, host->d_name, sizeof out->d_name);
    return out;
}

BIONIC_EXPORT struct linux_dirent* bionic_readdir(DIR* dir) {
    // As with the real readdir, the result is only valid until the next call.
    static __thread struct linux_dirent entry;
    struct dirent* host = readdir(dir);
    return host ? dirent_from_host(host, &entry) : NULL;
}
BIONIC_EXPORT struct linux_dirent* bionic_readdir64(DIR* dir) { return bionic_readdir(dir); }

BIONIC_EXPORT int bionic_readdir_r(DIR* dir, struct linux_dirent* entry, struct linux_dirent** result) {
    struct dirent* host = readdir(dir);
    *result = host ? dirent_from_host(host, entry) : NULL;
    return 0;
}

// --- Memory mappings -------------------------------------------------------------------

BIONIC_EXPORT void* bionic_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    int host = flags & (MAP_SHARED | MAP_PRIVATE | MAP_FIXED);  // 1, 2 and 0x10 on both
    if (flags & 0x20) host |= MAP_ANON;
    if (flags & 0x4000) host |= MAP_NORESERVE;
    // Executable mappings cannot be created by an iOS app; the request fails as it would
    // on a locked-down Android device.
    return mmap(address, length, protection, host, (flags & 0x20) ? -1 : fd, offset);
}
BIONIC_EXPORT void* bionic_mmap64(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    return bionic_mmap(address, length, protection, flags, fd, offset);
}

BIONIC_EXPORT int bionic_munmap(void* address, size_t length) { return munmap(address, length); }
BIONIC_EXPORT int bionic_mprotect(void* address, size_t length, int protection) {
    return mprotect(address, length, protection);
}
BIONIC_EXPORT int bionic_mlock(const void* address, size_t length) { return mlock(address, length); }
BIONIC_EXPORT int bionic_munlock(const void* address, size_t length) { return munlock(address, length); }

BIONIC_EXPORT int bionic_msync(void* address, size_t length, int flags) {
    int host = 0;
    if (flags & 1) host |= MS_ASYNC;
    if (flags & 2) host |= MS_INVALIDATE;
    if (flags & 4) host |= MS_SYNC;
    return msync(address, length, host);
}

BIONIC_EXPORT int bionic_madvise(void* address, size_t length, int advice) {
    if (advice > 4) return 0;  // Linux-only hints; 0..4 agree
    return madvise(address, length, advice);
}

// Linux can resize or move a mapping; iOS cannot. Shrinking is done in place; growing is
// only possible by moving, which works for private anonymous memory (the contents are
// copied) but not for a mapping of a file, whose identity is not known here.
BIONIC_EXPORT void* bionic_mremap(void* old, size_t old_size, size_t new_size, int flags, ...) {
    if (new_size <= old_size) {
        if (new_size < old_size) munmap((char*)old + new_size, old_size - new_size);
        return old;
    }
    bionic_set_errno(12 /* ENOMEM */);
    return MAP_FAILED;
}

// --- Other calls with differing structures -----------------------------------------------

struct linux_timeval {
    long tv_sec;
    long tv_usec;
};

BIONIC_EXPORT int bionic_gettimeofday(struct linux_timeval* out, void* timezone) {
    struct timeval host;
    int result = gettimeofday(&host, NULL);
    if (result == 0 && out != NULL) {
        out->tv_sec = host.tv_sec;
        out->tv_usec = host.tv_usec;
    }
    return result;
}

BIONIC_EXPORT int bionic_utimes(const char* path, const struct linux_timeval times[2]) {
    if (times == NULL) return utimes(path, NULL);
    struct timeval host[2] = {{times[0].tv_sec, (int)times[0].tv_usec}, {times[1].tv_sec, (int)times[1].tv_usec}};
    return utimes(path, host);
}

// Linux's sendfile copies between any two descriptors.
BIONIC_EXPORT ssize_t bionic_sendfile(int out, int in, off_t* offset, size_t count) {
    char buffer[64 * 1024];
    size_t total = 0;
    while (total < count) {
        size_t want = count - total < sizeof buffer ? count - total : sizeof buffer;
        ssize_t n = offset ? pread(in, buffer, want, *offset) : read(in, buffer, want);
        if (n < 0) return total > 0 ? (ssize_t)total : -1;
        if (n == 0) break;
        ssize_t written = write(out, buffer, (size_t)n);
        if (written < 0) return total > 0 ? (ssize_t)total : -1;
        if (offset) *offset += written;
        total += (size_t)written;
        if (written < n) break;
    }
    return (ssize_t)total;
}

// Fortified variants: the same call with the buffer's size, which is trusted here.
BIONIC_EXPORT ssize_t bionic___read_chk(int fd, void* buffer, size_t count, size_t buffer_size) {
    return read(fd, buffer, count);
}
BIONIC_EXPORT ssize_t bionic___write_chk(int fd, const void* buffer, size_t count, size_t buffer_size) {
    return write(fd, buffer, count);
}
