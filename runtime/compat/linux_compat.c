// Linux behaviour for Android native code running on Darwin: the system calls and
// libc functions that Darwin lacks or defines differently. Declarations are in
// include/a2i_linux_compat.h and the Linux-named headers beside it.

#include "a2i_linux_compat.h"

#include <dlfcn.h>
#include <limits.h>
#include <mntent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/un.h>

// The header redirects these names to the wrappers below; this file needs the real ones.
#undef getsockopt
#undef getxattr
#undef setxattr
#undef removexattr
#undef listxattr
#undef sendfile
#undef syscall
#undef socket
#undef socketpair
#undef uname

// --- Android's directory layout ------------------------------------------------------

// Where Android's top-level directories are on this system. ANDROID_ROOT is the bundled
// system directory (Android's /system) and ANDROID_DATA the app's writable container
// (Android's /data); both are set by the host before the runtime starts.
static const struct {
    const char *android;  // absolute Android directory
    const char *variable; // environment variable holding its base here
    const char *below;    // path below that base
} kMounts[] = {
    {"/system", "ANDROID_ROOT", ""},
    {"/apex", "ANDROID_ROOT", "/apex"},
    {"/vendor", "ANDROID_ROOT", "/vendor"},
    {"/product", "ANDROID_ROOT", "/product"},
    {"/system_ext", "ANDROID_ROOT", "/system_ext"},
    {"/odm", "ANDROID_ROOT", "/odm"},
    {"/data", "ANDROID_DATA", ""},
    {"/cache", "ANDROID_DATA", "/cache"},
    {"/sdcard", "ANDROID_DATA", "/media/0"},
    {"/storage/emulated/0", "ANDROID_DATA", "/media/0"},
    {"/storage", "ANDROID_DATA", "/media"},
    {"/mnt", "ANDROID_DATA", "/mnt"},
};

// Returns `path` itself, or its relocation written to `buffer` when it lies in one of
// Android's top-level directories.
const char *a2i_android_path(const char *path, char *buffer, size_t size) {
    if (path == NULL || path[0] != '/') {
        return path;
    }
    for (size_t i = 0; i < sizeof kMounts / sizeof kMounts[0]; i++) {
        size_t length = strlen(kMounts[i].android);
        if (strncmp(path, kMounts[i].android, length) != 0 || (path[length] != '/' && path[length] != 0)) {
            continue;
        }
        const char *base = getenv(kMounts[i].variable);
        if (base == NULL) {
            return path;
        }
        // The base itself may start with a mapped name (a container under /data, say):
        // paths that are already below it must not be relocated a second time.
        size_t base_length = strlen(base);
        if (strncmp(path, base, base_length) == 0 && (path[base_length] == '/' || path[base_length] == 0)) {
            return path;
        }
        int written = snprintf(buffer, size, "%s%s%s", base, kMounts[i].below, path + length);
        return written > 0 && (size_t)written < size ? buffer : path;
    }
    return path;
}

#define RELOCATED(path) a2i_android_path((path), (char[PATH_MAX]){0}, PATH_MAX)

// The path-taking libc functions, redefined for the library this file is linked into (see
// the header). Each relocates its path and forwards to libc's own function, found with
// RTLD_NEXT because the plain name now refers to the definition here.
#define REAL(name) ((__typeof__(&name))real_function(#name))

static void *real_function(const char *name) {
    void *function = dlsym(RTLD_NEXT, name);
    if (function == NULL) {
        fprintf(stderr, "a2i: libc has no %s\n", name);
        abort();
    }
    return function;
}

int open(const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    int mode = (flags & O_CREAT) ? va_arg(args, int) : 0;
    va_end(args);
    return REAL(open)(RELOCATED(path), flags, mode);
}

FILE *fopen(const char *path, const char *mode) { return REAL(fopen)(RELOCATED(path), mode); }
int stat(const char *path, struct stat *st) { return REAL(stat)(RELOCATED(path), st); }
int lstat(const char *path, struct stat *st) { return REAL(lstat)(RELOCATED(path), st); }
int access(const char *path, int mode) { return REAL(access)(RELOCATED(path), mode); }
DIR *opendir(const char *path) { return REAL(opendir)(RELOCATED(path)); }
int mkdir(const char *path, mode_t mode) { return REAL(mkdir)(RELOCATED(path), mode); }
int rmdir(const char *path) { return REAL(rmdir)(RELOCATED(path)); }
int unlink(const char *path) { return REAL(unlink)(RELOCATED(path)); }
int rename(const char *from, const char *to) { return REAL(rename)(RELOCATED(from), RELOCATED(to)); }
ssize_t readlink(const char *path, char *buffer, size_t size) {
    return REAL(readlink)(RELOCATED(path), buffer, size);
}
char *realpath(const char *path, char *resolved) { return REAL(realpath)(RELOCATED(path), resolved); }
int chmod(const char *path, mode_t mode) { return REAL(chmod)(RELOCATED(path), mode); }
int statvfs(const char *path, struct statvfs *st) { return REAL(statvfs)(RELOCATED(path), st); }
int truncate(const char *path, off_t length) { return REAL(truncate)(RELOCATED(path), length); }

// --- Sockets -------------------------------------------------------------------------

int a2i_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (level == SOL_SOCKET && (option == SO_DOMAIN || option == SO_PROTOCOL)) {
        if (value == NULL || length == NULL || *length < sizeof(int)) {
            errno = EINVAL;
            return -1;
        }
        int result;
        if (option == SO_DOMAIN) {
            struct sockaddr_storage address;
            socklen_t address_length = sizeof address;
            if (getsockname(fd, (struct sockaddr *)&address, &address_length) != 0) {
                return -1;  // EBADF / ENOTSOCK, as on Linux
            }
            result = address.ss_family;
        } else {
            // Darwin cannot report the protocol; confirm it is a socket and report the
            // default protocol for its type.
            socklen_t type_length = sizeof result;
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &result, &type_length) != 0) {
                return -1;
            }
            result = 0;
        }
        memcpy(value, &result, sizeof result);
        *length = sizeof result;
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_PEERCRED) {
        struct ucred peer;
        if (value == NULL || length == NULL || *length < sizeof peer) {
            errno = EINVAL;
            return -1;
        }
        if (getpeereid(fd, &peer.uid, &peer.gid) != 0) {
            return -1;
        }
        socklen_t pid_length = sizeof peer.pid;
        if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer.pid, &pid_length) != 0) {
            peer.pid = 0;
        }
        memcpy(value, &peer, sizeof peer);
        *length = sizeof peer;
        return 0;
    }
    return getsockopt(fd, level, option, value, length);
}

// Apply the SOCK_CLOEXEC/SOCK_NONBLOCK bits of a Linux socket type to a new descriptor.
static int apply_type_flags(int fd, int type) {
    if ((type & SOCK_CLOEXEC) != 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return -1;
    }
    if ((type & SOCK_NONBLOCK) != 0 && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
}

int a2i_socket(int domain, int type, int protocol) {
    int fd = socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
    if (fd >= 0 && apply_type_flags(fd, type) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int a2i_socketpair(int domain, int type, int protocol, int fds[2]) {
    if (socketpair(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol, fds) != 0) {
        return -1;
    }
    if (apply_type_flags(fds[0], type) != 0 || apply_type_flags(fds[1], type) != 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

// --- Extended attributes -------------------------------------------------------------

// Linux's XATTR_CREATE (1) and XATTR_REPLACE (2) are Darwin's 2 and 4.
static int xattr_options(int linux_flags) {
    return ((linux_flags & 1) ? XATTR_CREATE : 0) | ((linux_flags & 2) ? XATTR_REPLACE : 0);
}

ssize_t a2i_linux_getxattr(const char *path, const char *name, void *value, size_t size) {
    return getxattr(RELOCATED(path), name, value, size, 0, 0);
}

int a2i_linux_setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    return setxattr(RELOCATED(path), name, value, size, 0, xattr_options(flags));
}

int a2i_linux_removexattr(const char *path, const char *name) {
    return removexattr(RELOCATED(path), name, 0);
}

ssize_t a2i_linux_listxattr(const char *path, char *list, size_t size) {
    return listxattr(RELOCATED(path), list, size, 0);
}

ssize_t a2i_linux_fgetxattr(int fd, const char *name, void *value, size_t size) {
    return fgetxattr(fd, name, value, size, 0, 0);
}

int a2i_linux_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    return fsetxattr(fd, name, value, size, 0, xattr_options(flags));
}

int a2i_linux_fremovexattr(int fd, const char *name) {
    return fremovexattr(fd, name, 0);
}

ssize_t a2i_linux_flistxattr(int fd, char *list, size_t size) {
    return flistxattr(fd, list, size, 0);
}

// --- File data -----------------------------------------------------------------------

// Copy up to `count` bytes from in_fd to out_fd. With `offset`, read at *offset without
// moving in_fd's position and store the new offset back, as Linux does.
static ssize_t copy_between(int in_fd, off_t *offset, int out_fd, size_t count) {
    char buffer[64 * 1024];
    size_t total = 0;
    while (total < count) {
        size_t want = count - total < sizeof buffer ? count - total : sizeof buffer;
        ssize_t got = offset != NULL ? pread(in_fd, buffer, want, *offset) : read(in_fd, buffer, want);
        if (got < 0) {
            return total > 0 ? (ssize_t)total : -1;
        }
        if (got == 0) {
            break;
        }
        for (ssize_t written = 0; written < got;) {
            ssize_t n = write(out_fd, buffer + written, (size_t)(got - written));
            if (n < 0) {
                return total > 0 ? (ssize_t)total : -1;
            }
            written += n;
        }
        if (offset != NULL) {
            *offset += got;
        }
        total += (size_t)got;
    }
    return (ssize_t)total;
}

ssize_t a2i_linux_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    return copy_between(in_fd, offset, out_fd, count);
}

ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out, size_t length, unsigned int flags) {
    (void)flags;
    if (off_out != NULL) {  // positional output is only meaningful for regular files
        if (lseek(fd_out, *off_out, SEEK_SET) < 0) {
            return -1;
        }
    }
    ssize_t copied = copy_between(fd_in, off_in, fd_out, length);
    if (copied > 0 && off_out != NULL) {
        *off_out += copied;
    }
    return copied;
}

int posix_fallocate(int fd, off_t offset, off_t length) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return errno;
    }
    off_t end = offset + length;
    if (end <= st.st_size) {
        return 0;
    }
    // Reserve the blocks if the file system can; extending the file is what is required.
    fstore_t store = {F_ALLOCATEALL, F_PEOFPOSMODE, 0, end - st.st_size, 0};
    (void)fcntl(fd, F_PREALLOCATE, &store);
    return ftruncate(fd, end) == 0 ? 0 : errno;
}

// An iOS app cannot enumerate mounts, so the mount table reads as absent, which callers
// already handle (it is what they see when /proc/mounts cannot be opened).
FILE *setmntent(const char *path, const char *mode) {
    (void)path, (void)mode;
    errno = ENOENT;
    return NULL;
}

struct mntent *getmntent_r(FILE *fp, struct mntent *entry, char *buffer, int buffer_size) {
    (void)fp, (void)entry, (void)buffer, (void)buffer_size;
    return NULL;
}

int endmntent(FILE *fp) {
    if (fp != NULL) {
        fclose(fp);
    }
    return 1;
}

// --- Process and system identity -----------------------------------------------------

int a2i_linux_uname(struct utsname *name) {
    if (uname(name) != 0) {
        return -1;
    }
    // Keep the host name; present the kernel and machine the way an arm64 Android device does.
    strlcpy(name->sysname, "Linux", sizeof name->sysname);
    strlcpy(name->release, "6.1.0-philae", sizeof name->release);
    strlcpy(name->version, "#1 SMP PREEMPT", sizeof name->version);
    strlcpy(name->machine, "aarch64", sizeof name->machine);
    return 0;
}

pid_t gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}

long a2i_linux_syscall(long number, ...) {
    if (number == __NR_gettid) {
        return gettid();
    }
    errno = ENOSYS;
    return -1;
}

// iOS has no capability model for apps: report an empty set and refuse changes.
int capget(cap_user_header_t header, cap_user_data_t data) {
    if (header == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (data != NULL) {
        memset(data, 0, sizeof(*data) * _LINUX_CAPABILITY_U32S_3);
    }
    return 0;
}

int capset(cap_user_header_t header, const cap_user_data_t data) {
    (void)header, (void)data;
    errno = EPERM;
    return -1;
}

int prctl(int option, ...) {
    va_list args;
    va_start(args, option);
    unsigned long arg2 = va_arg(args, unsigned long);
    va_end(args);
    switch (option) {
        case PR_SET_NAME:
            return pthread_setname_np((const char *)arg2) == 0 ? 0 : -1;
        case PR_GET_NAME:
            // Linux thread names are at most 16 bytes including the terminator.
            return pthread_getname_np(pthread_self(), (char *)arg2, 16) == 0 ? 0 : -1;
        case PR_GET_DUMPABLE:
            return 0;
        case PR_SET_DUMPABLE:
        case PR_SET_VMA:  // naming anonymous mappings is purely diagnostic
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}
