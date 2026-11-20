// errno for Android code.
//
// Error numbers above 34 differ between Linux, whose numbering Android code is compiled
// with, and iOS. Android code reads and writes errno through __errno(), so it gets its
// own per-thread variable in Linux numbering. The iOS functions that the bionic library
// forwards to keep setting the real errno; that is treated as "a new error is pending":
// on each access the real errno, if set, is translated into Android's variable and
// cleared. What Android code writes itself (errno = 0 before strtol) therefore sticks
// until the next failing call.

#include <errno.h>

#include "bionic_internal.h"

// Linux error number for each iOS one, where it differs or is above 34.
static const unsigned char kFromHost[] = {
        [EDEADLK] = 35,       [EAGAIN] = 11,         [EINPROGRESS] = 115, [EALREADY] = 114,
        [ENOTSOCK] = 88,      [EDESTADDRREQ] = 89,   [EMSGSIZE] = 90,     [EPROTOTYPE] = 91,
        [ENOPROTOOPT] = 92,   [EPROTONOSUPPORT] = 93, [ESOCKTNOSUPPORT] = 94, [ENOTSUP] = 95,
        [EPFNOSUPPORT] = 96,  [EAFNOSUPPORT] = 97,   [EADDRINUSE] = 98,   [EADDRNOTAVAIL] = 99,
        [ENETDOWN] = 100,     [ENETUNREACH] = 101,   [ENETRESET] = 102,   [ECONNABORTED] = 103,
        [ECONNRESET] = 104,   [ENOBUFS] = 105,       [EISCONN] = 106,     [ENOTCONN] = 107,
        [ESHUTDOWN] = 108,    [ETOOMANYREFS] = 109,  [ETIMEDOUT] = 110,   [ECONNREFUSED] = 111,
        [ELOOP] = 40,         [ENAMETOOLONG] = 36,   [EHOSTDOWN] = 112,   [EHOSTUNREACH] = 113,
        [ENOTEMPTY] = 39,     [EPROCLIM] = 11,       [EUSERS] = 87,       [EDQUOT] = 122,
        [ESTALE] = 116,       [EREMOTE] = 66,        [EBADRPC] = 5,       [ERPCMISMATCH] = 5,
        [EPROGUNAVAIL] = 5,   [EPROGMISMATCH] = 5,   [EPROCUNAVAIL] = 5,  [ENOLCK] = 37,
        [ENOSYS] = 38,        [EFTYPE] = 22,         [EAUTH] = 13,        [ENEEDAUTH] = 13,
        [EPWROFF] = 5,        [EDEVERR] = 5,         [EOVERFLOW] = 75,    [EBADEXEC] = 8,
        [EBADARCH] = 8,       [ESHLIBVERS] = 8,      [EBADMACHO] = 8,     [ECANCELED] = 125,
        [EIDRM] = 43,         [ENOMSG] = 42,         [EILSEQ] = 84,       [ENOATTR] = 61,
        [EBADMSG] = 74,       [EMULTIHOP] = 72,      [ENODATA] = 61,      [ENOLINK] = 67,
        [ENOSR] = 63,         [ENOSTR] = 60,         [EPROTO] = 71,       [ETIME] = 62,
        [EOPNOTSUPP] = 95,    [ENOPOLICY] = 1,       [ENOTRECOVERABLE] = 131, [EOWNERDEAD] = 130,
        [EQFULL] = 105,
};

int bionic_errno_from_host(int host) {
    if (host < 0 || host >= (int)sizeof kFromHost) return host < 0 ? host : 5 /* EIO */;
    return kFromHost[host] != 0 ? kFromHost[host] : host;
}

int bionic_errno_to_host(int android) {
    if (android == 35) return EDEADLK;
    if (android == 11) return EAGAIN;
    if (android <= 34) return android;
    for (int host = 35; host < (int)sizeof kFromHost; host++) {
        if (kFromHost[host] == android) return host;
    }
    return EIO;
}

static __thread int tErrno;

BIONIC_EXPORT int* bionic___errno(void) {
    if (errno != 0) {
        tErrno = bionic_errno_from_host(errno);
        errno = 0;
    }
    return &tErrno;
}

void bionic_set_errno(int android) {
    errno = 0;
    tErrno = android;
}
