// Threads for Android code.
//
// The pthread types an Android library allocates are bionic's: a mutex is 40 bytes, a
// condition variable 48, a read-write lock 56. The iOS types are larger and laid out
// differently, so they cannot live in that storage. Each bionic object instead refers to
// an iOS object created on first use (bionic's static initializers leave the word used
// for that zero). The first word keeps what bionic's initializers put there: the mutex
// type.
//
// These functions return error numbers rather than setting errno, so results are
// translated to Android's numbering.

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bionic_internal.h"

#define RESULT(host_error) bionic_errno_from_host(host_error)

// A bionic synchronisation object, as far as this library uses its storage. bionic's
// types are arrays of 32-bit words, so they are only aligned to four bytes, which rules
// out keeping (and atomically updating) a pointer in them; the second word instead holds
// a handle to the iOS object.
typedef struct {
    int state;               // bionic's: bits 14-15 hold the mutex type; our condition variables keep the clock here
    _Atomic uint32_t handle;  // 0: no iOS object yet
} bionic_object;

// Handles are indices into a table that only grows, in chunks that never move, so a
// lookup needs no lock.
enum { kChunkBits = 10, kChunkSize = 1 << kChunkBits, kChunks = 1 << 12 };
static void** _Atomic gChunks[kChunks];
static pthread_mutex_t gHandleLock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t gNextHandle = 1;
static uint32_t gFreeHandles[256];
static int gFreeCount;

static void* object_at(uint32_t handle) {
    return atomic_load(&gChunks[handle >> kChunkBits])[handle & (kChunkSize - 1)];
}

static uint32_t new_handle(void* host) {
    pthread_mutex_lock(&gHandleLock);
    uint32_t handle = gFreeCount > 0 ? gFreeHandles[--gFreeCount] : gNextHandle++;
    if (handle >> kChunkBits >= kChunks) abort();  // four million live objects
    void** chunk = atomic_load(&gChunks[handle >> kChunkBits]);
    if (chunk == NULL) {
        chunk = calloc(kChunkSize, sizeof(void*));
        atomic_store(&gChunks[handle >> kChunkBits], chunk);
    }
    chunk[handle & (kChunkSize - 1)] = host;
    pthread_mutex_unlock(&gHandleLock);
    return handle;
}

static void free_handle(uint32_t handle) {
    pthread_mutex_lock(&gHandleLock);
    if (gFreeCount < (int)(sizeof gFreeHandles / sizeof gFreeHandles[0])) gFreeHandles[gFreeCount++] = handle;
    pthread_mutex_unlock(&gHandleLock);
}

// The iOS object behind a bionic one, created by `create` if there is none yet.
static void* host_of(void* object, void* (*create)(int state)) {
    bionic_object* o = object;
    uint32_t handle = atomic_load(&o->handle);
    if (handle == 0) {
        void* created = create(o->state);
        uint32_t mine = new_handle(created);
        if (atomic_compare_exchange_strong(&o->handle, &handle, mine)) {
            return created;
        }
        free_handle(mine);  // another thread was first; an unused iOS object needs no destroying
        free(created);
    }
    return object_at(handle);
}

// Detaches and returns the iOS object, if one was created.
static void* take_host(void* object) {
    bionic_object* o = object;
    uint32_t handle = atomic_exchange(&o->handle, 0);
    if (handle == 0) return NULL;
    void* host = object_at(handle);
    free_handle(handle);
    return host;
}

// --- Mutexes -----------------------------------------------------------------------------

static void* create_mutex(int state) {
    pthread_mutex_t* mutex = malloc(sizeof *mutex);
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    int type = state >> 14 & 3;  // 0 normal, 1 recursive, 2 error-checking
    pthread_mutexattr_settype(&attributes, type == 1 ? PTHREAD_MUTEX_RECURSIVE
                                         : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return mutex;
}
#define MUTEX(m) ((pthread_mutex_t*)host_of(m, create_mutex))

// bionic's pthread_mutexattr_t is a long holding the type in its low bits.
BIONIC_EXPORT int bionic_pthread_mutexattr_init(long* attributes) { *attributes = 0; return 0; }
BIONIC_EXPORT int bionic_pthread_mutexattr_destroy(long* attributes) { return 0; }
BIONIC_EXPORT int bionic_pthread_mutexattr_settype(long* attributes, int type) {
    *attributes = (*attributes & ~3L) | (type & 3);
    return 0;
}
BIONIC_EXPORT int bionic_pthread_mutexattr_gettype(const long* attributes, int* type) {
    *type = (int)(*attributes & 3);
    return 0;
}
BIONIC_EXPORT int bionic_pthread_mutexattr_setpshared(long* attributes, int shared) { return 0; }

BIONIC_EXPORT int bionic_pthread_mutex_init(void* mutex, const long* attributes) {
    memset(mutex, 0, 40);
    ((bionic_object*)mutex)->state = attributes ? (int)(*attributes & 3) << 14 : 0;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_mutex_destroy(void* mutex) {
    pthread_mutex_t* host = take_host(mutex);
    if (host != NULL) {
        pthread_mutex_destroy(host);
        free(host);
    }
    return 0;
}
BIONIC_EXPORT int bionic_pthread_mutex_lock(void* mutex) { return RESULT(pthread_mutex_lock(MUTEX(mutex))); }
BIONIC_EXPORT int bionic_pthread_mutex_trylock(void* mutex) { return RESULT(pthread_mutex_trylock(MUTEX(mutex))); }
BIONIC_EXPORT int bionic_pthread_mutex_unlock(void* mutex) { return RESULT(pthread_mutex_unlock(MUTEX(mutex))); }

// --- Condition variables ---------------------------------------------------------------

static void* create_cond(int state) {
    pthread_cond_t* cond = malloc(sizeof *cond);
    pthread_cond_init(cond, NULL);
    return cond;
}
#define COND(c) ((pthread_cond_t*)host_of(c, create_cond))
enum { kCondMonotonic = 2 };

BIONIC_EXPORT int bionic_pthread_condattr_init(long* attributes) { *attributes = 0; return 0; }
BIONIC_EXPORT int bionic_pthread_condattr_destroy(long* attributes) { return 0; }
BIONIC_EXPORT int bionic_pthread_condattr_setclock(long* attributes, int clock) {
    *attributes = clock == 1 /* CLOCK_MONOTONIC */ ? kCondMonotonic : 0;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_condattr_setpshared(long* attributes, int shared) { return 0; }

BIONIC_EXPORT int bionic_pthread_cond_init(void* cond, const long* attributes) {
    memset(cond, 0, 48);
    ((bionic_object*)cond)->state = attributes ? (int)*attributes : 0;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_cond_destroy(void* cond) {
    pthread_cond_t* host = take_host(cond);
    if (host != NULL) {
        pthread_cond_destroy(host);
        free(host);
    }
    return 0;
}
BIONIC_EXPORT int bionic_pthread_cond_signal(void* cond) { return RESULT(pthread_cond_signal(COND(cond))); }
BIONIC_EXPORT int bionic_pthread_cond_broadcast(void* cond) { return RESULT(pthread_cond_broadcast(COND(cond))); }
BIONIC_EXPORT int bionic_pthread_cond_wait(void* cond, void* mutex) {
    return RESULT(pthread_cond_wait(COND(cond), MUTEX(mutex)));
}
BIONIC_EXPORT int bionic_pthread_cond_timedwait(void* cond, void* mutex, const struct timespec* deadline) {
    if (((bionic_object*)cond)->state & kCondMonotonic) {
        // The deadline is on the monotonic clock, which iOS condition variables cannot
        // wait on; wait for the time remaining instead.
        struct timespec now, remaining;
        clock_gettime(CLOCK_MONOTONIC, &now);
        remaining.tv_sec = deadline->tv_sec - now.tv_sec;
        remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
        if (remaining.tv_nsec < 0) {
            remaining.tv_sec--;
            remaining.tv_nsec += 1000000000;
        }
        if (remaining.tv_sec < 0) remaining.tv_sec = remaining.tv_nsec = 0;
        return RESULT(pthread_cond_timedwait_relative_np(COND(cond), MUTEX(mutex), &remaining));
    }
    return RESULT(pthread_cond_timedwait(COND(cond), MUTEX(mutex), deadline));
}

// --- Read-write locks ------------------------------------------------------------------

static void* create_rwlock(int state) {
    pthread_rwlock_t* lock = malloc(sizeof *lock);
    pthread_rwlock_init(lock, NULL);
    return lock;
}
#define RWLOCK(l) ((pthread_rwlock_t*)host_of(l, create_rwlock))

BIONIC_EXPORT int bionic_pthread_rwlock_init(void* lock, const void* attributes) {
    memset(lock, 0, 56);
    return 0;
}
BIONIC_EXPORT int bionic_pthread_rwlock_destroy(void* lock) {
    pthread_rwlock_t* host = take_host(lock);
    if (host != NULL) {
        pthread_rwlock_destroy(host);
        free(host);
    }
    return 0;
}
BIONIC_EXPORT int bionic_pthread_rwlock_rdlock(void* lock) { return RESULT(pthread_rwlock_rdlock(RWLOCK(lock))); }
BIONIC_EXPORT int bionic_pthread_rwlock_tryrdlock(void* lock) { return RESULT(pthread_rwlock_tryrdlock(RWLOCK(lock))); }
BIONIC_EXPORT int bionic_pthread_rwlock_wrlock(void* lock) { return RESULT(pthread_rwlock_wrlock(RWLOCK(lock))); }
BIONIC_EXPORT int bionic_pthread_rwlock_trywrlock(void* lock) { return RESULT(pthread_rwlock_trywrlock(RWLOCK(lock))); }
BIONIC_EXPORT int bionic_pthread_rwlock_unlock(void* lock) { return RESULT(pthread_rwlock_unlock(RWLOCK(lock))); }

// --- Once ----------------------------------------------------------------------------------

// bionic's pthread_once_t is an int that starts at 0.
BIONIC_EXPORT int bionic_pthread_once(_Atomic int* once, void (*routine)(void)) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
    enum { kNever, kRunning, kDone };
    if (atomic_load(once) == kDone) return 0;
    pthread_mutex_lock(&lock);
    while (atomic_load(once) == kRunning) pthread_cond_wait(&done, &lock);
    if (atomic_load(once) == kNever) {
        atomic_store(once, kRunning);
        pthread_mutex_unlock(&lock);
        routine();
        pthread_mutex_lock(&lock);
        atomic_store(once, kDone);
        pthread_cond_broadcast(&done);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

// --- Thread-specific data: bionic's key is an int, iOS's an unsigned long ------------------

BIONIC_EXPORT int bionic_pthread_key_create(int* key, void (*destructor)(void*)) {
    pthread_key_t host;
    int result = pthread_key_create(&host, destructor);
    if (result == 0) *key = (int)host;
    return RESULT(result);
}
BIONIC_EXPORT int bionic_pthread_key_delete(int key) { return RESULT(pthread_key_delete((pthread_key_t)key)); }
BIONIC_EXPORT void* bionic_pthread_getspecific(int key) { return pthread_getspecific((pthread_key_t)key); }
BIONIC_EXPORT int bionic_pthread_setspecific(int key, const void* value) {
    return RESULT(pthread_setspecific((pthread_key_t)key, value));
}

// --- Threads -------------------------------------------------------------------------------

// bionic's pthread_attr_t.
typedef struct {
    uint32_t flags;  // bit 0: detached
    void* stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} bionic_attr;

BIONIC_EXPORT int bionic_pthread_attr_init(bionic_attr* attributes) {
    memset(attributes, 0, sizeof *attributes);
    attributes->stack_size = 1024 * 1024;  // bionic's default
    attributes->guard_size = 16384;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_destroy(bionic_attr* attributes) { return 0; }
BIONIC_EXPORT int bionic_pthread_attr_setstacksize(bionic_attr* attributes, size_t size) {
    attributes->stack_size = size;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_getstacksize(const bionic_attr* attributes, size_t* size) {
    *size = attributes->stack_size;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_setdetachstate(bionic_attr* attributes, int state) {
    attributes->flags = (attributes->flags & ~1u) | (state == 1 /* PTHREAD_CREATE_DETACHED */ ? 1 : 0);
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_getdetachstate(const bionic_attr* attributes, int* state) {
    *state = attributes->flags & 1;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_setguardsize(bionic_attr* attributes, size_t size) {
    attributes->guard_size = size;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_getstack(const bionic_attr* attributes, void** base, size_t* size) {
    *base = attributes->stack_base;
    *size = attributes->stack_size;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_attr_setschedparam(bionic_attr* attributes, const void* param) { return 0; }
BIONIC_EXPORT int bionic_pthread_attr_setschedpolicy(bionic_attr* attributes, int policy) { return 0; }
BIONIC_EXPORT int bionic_pthread_attr_setscope(bionic_attr* attributes, int scope) { return 0; }

// The attributes of a running thread; what callers want from it is the stack's extent.
BIONIC_EXPORT int bionic_pthread_getattr_np(long thread, bionic_attr* attributes) {
    bionic_pthread_attr_init(attributes);
    pthread_t host = (pthread_t)thread;
    size_t size = pthread_get_stacksize_np(host);
    attributes->stack_size = size;
    attributes->stack_base = (char*)pthread_get_stackaddr_np(host) - size;  // iOS reports the top
    return 0;
}

BIONIC_EXPORT int bionic_pthread_create(long* thread, const bionic_attr* attributes, void* (*start)(void*),
                                        void* argument) {
    pthread_attr_t host_attributes;
    pthread_attr_init(&host_attributes);
    if (attributes != NULL) {
        if (attributes->stack_size >= PTHREAD_STACK_MIN) {
            // (iOS wants a multiple of the page size.)
            pthread_attr_setstacksize(&host_attributes, (attributes->stack_size + 16383) & ~(size_t)16383);
        }
        if (attributes->flags & 1) pthread_attr_setdetachstate(&host_attributes, PTHREAD_CREATE_DETACHED);
    }
    pthread_t host;
    int result = pthread_create(&host, &host_attributes, start, argument);
    pthread_attr_destroy(&host_attributes);
    if (result == 0) *thread = (long)host;
    return RESULT(result);
}

BIONIC_EXPORT long bionic_pthread_self(void) { return (long)pthread_self(); }
BIONIC_EXPORT int bionic_pthread_equal(long a, long b) { return a == b; }
BIONIC_EXPORT int bionic_pthread_join(long thread, void** result) { return RESULT(pthread_join((pthread_t)thread, result)); }
BIONIC_EXPORT int bionic_pthread_detach(long thread) { return RESULT(pthread_detach((pthread_t)thread)); }
BIONIC_EXPORT void bionic_pthread_exit(void* result) { pthread_exit(result); }
BIONIC_EXPORT int bionic_pthread_kill(long thread, int signal) { return RESULT(pthread_kill((pthread_t)thread, signal)); }
BIONIC_EXPORT int bionic_pthread_setname_np(long thread, const char* name) {
    // iOS can only name the calling thread.
    return (pthread_t)thread == pthread_self() ? RESULT(pthread_setname_np(name)) : 0;
}
BIONIC_EXPORT int bionic_pthread_getname_np(long thread, char* name, size_t size) {
    return RESULT(pthread_getname_np((pthread_t)thread, name, size));
}
BIONIC_EXPORT int bionic_pthread_setschedparam(long thread, int policy, const void* param) { return 0; }
BIONIC_EXPORT int bionic_pthread_getschedparam(long thread, int* policy, int* param) {
    *policy = 0;
    *param = 0;
    return 0;
}
BIONIC_EXPORT int bionic_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) { return 0; }
BIONIC_EXPORT int bionic___register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void* dso) {
    return 0;  // an iOS app never forks
}

// Signal masks: bionic's sigset_t is 64 bits, of which iOS understands the low 32.
BIONIC_EXPORT int bionic_pthread_sigmask(int how, const uint64_t* set, uint64_t* old) {
    sigset_t host_set = set ? (sigset_t)*set : 0, host_old = 0;
    // SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK are 0, 1, 2 on Linux and 1, 2, 3 on iOS.
    int result = pthread_sigmask(how + 1, set ? &host_set : NULL, &host_old);
    if (old != NULL) *old = host_old;
    return RESULT(result);
}
BIONIC_EXPORT int bionic_sigprocmask(int how, const uint64_t* set, uint64_t* old) {
    return bionic_pthread_sigmask(how, set, old);
}
BIONIC_EXPORT int bionic_sigemptyset(uint64_t* set) { *set = 0; return 0; }
BIONIC_EXPORT int bionic_sigfillset(uint64_t* set) { *set = ~0ULL; return 0; }
BIONIC_EXPORT int bionic_sigaddset(uint64_t* set, int signal) { *set |= 1ULL << (signal - 1); return 0; }
BIONIC_EXPORT int bionic_sigdelset(uint64_t* set, int signal) { *set &= ~(1ULL << (signal - 1)); return 0; }
BIONIC_EXPORT int bionic_sigismember(const uint64_t* set, int signal) { return (*set >> (signal - 1)) & 1; }

// The kernel's id for the calling thread.
BIONIC_EXPORT int bionic_gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (int)id;
}

// The CPUs a thread may run on: all of them.
BIONIC_EXPORT int bionic_sched_getaffinity(int pid, size_t size, void* mask) {
    memset(mask, 0, size);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < cpus && (size_t)i < size * 8; i++) ((unsigned char*)mask)[i / 8] |= 1u << (i % 8);
    return 0;
}
BIONIC_EXPORT int bionic_sched_setaffinity(int pid, size_t size, const void* mask) { return 0; }
