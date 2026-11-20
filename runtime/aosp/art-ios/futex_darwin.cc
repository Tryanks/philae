// futex(2) for ART on Darwin. ART's monitors and thread suspension are written directly
// against futexes; Darwin's equivalent primitive is os_sync_wait_on_address.

#include <linux/futex.h>

#include <errno.h>
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#include <stdint.h>

namespace {

constexpr uint64_t kNanosPerSecond = 1000000000ull;

uint64_t ToNanos(const timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * kNanosPerSecond + static_cast<uint64_t>(ts.tv_nsec);
}

int Wait(void* addr, int expected, const uint64_t* relative_ns) {
    const uint64_t value = static_cast<uint32_t>(expected);
    int result;
    if (relative_ns == nullptr) {
        result = os_sync_wait_on_address(addr, value, sizeof(int), OS_SYNC_WAIT_ON_ADDRESS_NONE);
    } else if (*relative_ns == 0) {
        errno = ETIMEDOUT;
        return -1;
    } else {
        result = os_sync_wait_on_address_with_timeout(addr, value, sizeof(int),
                                                      OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                      OS_CLOCK_MACH_ABSOLUTE_TIME, *relative_ns);
    }
    // A non-negative result is a wake-up (or the value had already changed); either way
    // callers re-check their condition, exactly as after a successful FUTEX_WAIT.
    return result >= 0 ? 0 : -1;
}

int Wake(void* addr, bool all) {
    int result = all ? os_sync_wake_by_address_all(addr, sizeof(int), OS_SYNC_WAKE_BY_ADDRESS_NONE)
                     : os_sync_wake_by_address_any(addr, sizeof(int), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    if (result != 0 && errno != ENOENT) {  // ENOENT: nobody was waiting
        return -1;
    }
    return result == 0 ? 1 : 0;
}

}  // namespace

int a2i_futex(volatile int* uaddr, int op, int val, const timespec* timeout,
              volatile int* /*uaddr2*/, int /*val3*/) {
    void* addr = const_cast<int*>(uaddr);
    switch (op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) {
        case FUTEX_WAIT: {
            if (timeout == nullptr) {
                return Wait(addr, val, nullptr);
            }
            uint64_t relative = ToNanos(*timeout);
            return Wait(addr, val, &relative);
        }
        case FUTEX_WAIT_BITSET: {
            // Unlike FUTEX_WAIT, the timeout is an absolute deadline on the selected clock.
            if (timeout == nullptr) {
                return Wait(addr, val, nullptr);
            }
            timespec now;
            clock_gettime((op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
            uint64_t deadline = ToNanos(*timeout), current = ToNanos(now);
            uint64_t relative = deadline > current ? deadline - current : 0;
            return Wait(addr, val, &relative);
        }
        case FUTEX_WAKE:
            return Wake(addr, /*all=*/val != 1);
        case FUTEX_REQUEUE: {
            // ART moves condition-variable waiters onto the mutex's futex so they wake one
            // at a time as the mutex is released. Darwin cannot requeue, so wake them here
            // and let them contend for the mutex. `timeout` carries the requeue count.
            intptr_t count = reinterpret_cast<intptr_t>(timeout);
            return Wake(addr, /*all=*/count != 1);
        }
        default:
            errno = ENOSYS;
            return -1;
    }
}
