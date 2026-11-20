// Linux/bionic source compatibility for Android native code built for Darwin.
//
// Force-included into Android libraries that are written against bionic. It only covers
// spellings: names that Linux has and Darwin provides under another name or by default.
// Behavioural gaps (system calls Darwin lacks) are implemented in linux_compat.c.
#pragma once
#if defined(__APPLE__) && !defined(__ASSEMBLER__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

// Large-file variants: every Darwin file API is already 64-bit.
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define fstatat64 fstatat
#define statvfs64 statvfs
#define fstatvfs64 fstatvfs
#define dirent64 dirent
#define readdir64 readdir
#define open64 open
#define mmap64 mmap
#define flock64 flock
#define F_GETLK64 F_GETLK
#define F_SETLK64 F_SETLK
#define F_SETLKW64 F_SETLKW
#ifndef __cplusplus
typedef off_t off64_t;
#endif

#include <sys/socket.h>
#include <sys/xattr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Socket options that exist only on Linux get numbers outside Darwin's range and are
// answered by a2i_getsockopt(); every other option goes straight to the kernel.
#define SO_DOMAIN 0x7a320001
#define SO_PROTOCOL 0x7a320002
int a2i_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
#define getsockopt a2i_getsockopt

// Extended attributes with Linux's signatures (Darwin's take extra position/options
// arguments). Named explicitly rather than shadowing Darwin's functions, because code
// built for Darwin in the same library may call those.
ssize_t a2i_linux_fgetxattr(int fd, const char *name, void *value, size_t size);
int a2i_linux_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags);
int a2i_linux_fremovexattr(int fd, const char *name);
ssize_t a2i_linux_flistxattr(int fd, char *list, size_t size);

// Linux's path-based xattr calls, under their own names in Linux-only sources.
ssize_t a2i_linux_getxattr(const char *path, const char *name, void *value, size_t size);
int a2i_linux_setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int a2i_linux_removexattr(const char *path, const char *name);
ssize_t a2i_linux_listxattr(const char *path, char *list, size_t size);
#define getxattr a2i_linux_getxattr
#define setxattr a2i_linux_setxattr
#define removexattr a2i_linux_removexattr
#define listxattr a2i_linux_listxattr

// Peer credentials of a Unix socket (SO_PEERCRED).
struct ucred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};
#define SO_PEERCRED 0x7a320003

// Linux sendfile/splice: copy between descriptors, advancing *offset when given.
ssize_t a2i_linux_sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
#ifndef _ALLBSD_SOURCE  // sources compiled with their BSD branches call Darwin's sendfile
#define sendfile a2i_linux_sendfile
#endif
ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out, size_t length, unsigned int flags);

// Returns an errno value, not -1.
int posix_fallocate(int fd, off_t offset, off_t length);
#define posix_fallocate64 posix_fallocate

pid_t gettid(void);

// Android code identifies its platform through uname(): os.name must be "Linux" (the
// class library selects its file system implementation by it) and os.arch "aarch64".
int a2i_linux_uname(struct utsname *name);
#define uname(name) a2i_linux_uname(name)

// Linux system call numbers (arm64) that Android code passes to syscall() directly.
// Darwin's syscall() must never see them, so the call is routed to an emulation.
#define __NR_gettid 178
long a2i_linux_syscall(long number, ...);
#define syscall a2i_linux_syscall

#ifdef __cplusplus
}
#endif

// Linux lets the socket type carry these flags. They get bits Darwin does not use in a
// socket type, and a2i_socket()/a2i_socketpair() apply them with fcntl().
#define SOCK_CLOEXEC 0x10000000
#define SOCK_NONBLOCK 0x20000000
#ifdef __cplusplus
extern "C" {
#endif
int a2i_socket(int domain, int type, int protocol);
int a2i_socketpair(int domain, int type, int protocol, int fds[2]);
#ifdef __cplusplus
}
#endif
#define socket(domain, type, protocol) a2i_socket((domain), (type), (protocol))
#define socketpair(domain, type, protocol, fds) a2i_socketpair((domain), (type), (protocol), (fds))

// Linux-only constants. Where Darwin has no counterpart the value is either harmless as
// a no-op (hints) or a number Darwin rejects (options, errno values Darwin never returns).
#define O_DIRECT 0            /* caching hint */
#define MAP_POPULATE 0        /* prefault hint */
#define ENONET 1064
#define SOL_PACKET 263
#define IP_MULTICAST_ALL 0x7a320010
#define SPLICE_F_MOVE 1
#define SPLICE_F_NONBLOCK 2
#define SPLICE_F_MORE 4
/* statvfs f_flag bits; Darwin only ever sets ST_RDONLY and ST_NOSUID. */
#define ST_NODEV 4
#define ST_NOEXEC 8
#define ST_SYNCHRONOUS 16
#define ST_MANDLOCK 64
#define ST_NOATIME 1024
#define ST_NODIRATIME 2048
#define ST_RELATIME 4096

// Android code is full of absolute paths (/system/fonts, /data/misc, /apex/...). Those
// directories live inside the app's bundle and container on iOS. linux_compat.c therefore
// defines the path-taking libc functions (open, stat, fopen, ...) itself: linked into a
// library, those definitions take precedence over libc's for every call in that library,
// relocate the path, and forward to the real function. No declarations are needed here.
#ifdef __cplusplus
extern "C"
#endif
const char *a2i_android_path(const char *path, char *buffer, size_t size);

// Darwin spells the nanosecond timestamps differently and takes char* in mincore().
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#define mincore(address, length, vector) mincore((address), (length), (char *)(vector))

// Darwin has no data-only sync; a full fsync is the conservative equivalent.
#define fdatasync fsync

// bionic's <unistd.h> macro for retrying system calls interrupted by a signal.
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)                \
    ({                                         \
        __typeof__(exp) _rc;                   \
        do {                                   \
            _rc = (exp);                       \
        } while (_rc == -1 && errno == EINTR); \
        _rc;                                   \
    })
#endif

#endif
