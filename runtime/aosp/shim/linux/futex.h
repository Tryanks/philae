// The slice of Linux's futex interface that ART uses, provided on Darwin by
// art-ios/futex_darwin.cc on top of os_sync_wait_on_address (iOS 17.4+).
#pragma once
#include <time.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_BITSET_MATCH_ANY 0xffffffff

#ifdef __cplusplus
extern "C"
#endif
// Same contract as the futex(2) system call for the operations above: 0 or a positive
// count on success, -1 with errno set (EAGAIN, EINTR, ETIMEDOUT) otherwise.
int a2i_futex(volatile int *uaddr, int op, int val, const struct timespec *timeout,
              volatile int *uaddr2, int val3);
