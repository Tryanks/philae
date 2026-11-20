// A native library built the way an Android app's is (arm64, Android's ABI, stack
// protectors on), exercising what the ELF-to-Mach-O converter and the bionic
// compatibility library have to get right. Built without Android's headers, so the C
// library functions it uses are declared here.

typedef unsigned long size_t;
typedef __builtin_va_list va_list;

void* malloc(size_t);
void free(void*);
size_t strlen(const char*);
char* strcpy(char*, const char*);
int strcmp(const char*, const char*);
void* memset(void*, int, size_t);
int snprintf(char*, size_t, const char*, ...);
int vsnprintf(char*, size_t, const char*, va_list);
int sscanf(const char*, const char*, ...);
int* __errno(void);
int open(const char*, int, ...);
int close(int);
long write(int, const void*, size_t);
long read(int, void*, size_t);
int unlink(const char*);
double strtod(const char*, char**);
long strtol(const char*, char**, int);
int pthread_mutex_lock(void*);
int pthread_mutex_unlock(void*);
int pthread_create(long*, const void*, void* (*)(void*), void*);
int pthread_join(long, void**);
int pthread_once(int*, void (*)(void));
int pthread_cond_timedwait(void*, void*, const void*);
int clock_gettime(int, void*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
int __android_log_print(int, const char*, const char*, ...);

struct stat_linux {
    unsigned long st_dev, st_ino;
    unsigned int st_mode, st_nlink, st_uid, st_gid;
    unsigned long st_rdev, pad1;
    long st_size;
    int st_blksize, pad2;
    long st_blocks;
    long times[6];
    unsigned int unused[2];
};
int stat(const char*, struct stat_linux*);

// --- Initializers and data relocations ---------------------------------------------------

static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = 42; }
int test_initializer(void) { return initialized; }

static const char* const kNames[] = {"zero", "one", "two"};  // pointers in data: relocated
static int (*const kCompare)(const char*, const char*) = strcmp;  // a function pointer to an import
const char* test_data_pointer(int i) { return kNames[i]; }
int test_import_pointer(void) { return kCompare("a", "a") == 0 && kCompare("a", "b") < 0; }

int exported_counter = 7;  // an exported variable, reached through the GOT
int test_global(void) { return ++exported_counter; }

// --- Stack protector (reads the guard through the thread pointer) ----------------------

int test_stack_guard(const char* text) {
    char buffer[64];
    strcpy(buffer, text);
    return (int)strlen(buffer);
}

// --- Variadic calls into the C library --------------------------------------------------

int test_format(char* out, size_t size) {
    return snprintf(out, size, "%s|%d|%5.2f|%lld|%c|%x|%-4d|%05d|%.3s|%%", "str", -42, 3.14159, 1LL << 40, 'x',
                    0xbeefu, 7, 42, "abcdef");
}

// More arguments than fit in registers: the rest are on the stack.
int test_format_many(char* out, size_t size) {
    return snprintf(out, size, "%d %d %d %d %d %d %d %d %d %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", 1,
                    2, 3, 4, 5, 6, 7, 8, 9, 10, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5);
}

static int forward(char* out, size_t size, const char* format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int n = vsnprintf(out, size, format, arguments);
    __builtin_va_end(arguments);
    return n;
}
int test_va_list(char* out, size_t size) { return forward(out, size, "%s-%d-%g", "va", 9, 0.25); }

int test_scan(void) {
    int a = 0;
    double b = 0;
    char word[16];
    int n = sscanf("12 3.5 hello", "%d %lf %15s", &a, &b, word);
    return n == 3 && a == 12 && b == 3.5 && strcmp(word, "hello") == 0;
}

int test_log(void) { return __android_log_print(4, "a2i.test", "hello from %s code, %d", "Android", 64); }

// --- Files, with Linux's flags, structures and error numbers ----------------------------

int test_file(const char* path) {
    int fd = open(path, 0101 | 01000 /* O_WRONLY | O_CREAT | O_TRUNC */, 0600);
    if (fd < 0) return -1;
    if (write(fd, "0123456789", 10) != 10) return -2;
    close(fd);
    struct stat_linux info;
    memset(&info, 0xff, sizeof info);
    if (stat(path, &info) != 0) return -3;
    if (info.st_size != 10 || (info.st_mode & 0170000) != 0100000) return -4;
    fd = open(path, 0 /* O_RDONLY */);
    char buffer[16] = {0};
    if (read(fd, buffer, sizeof buffer) != 10 || strcmp(buffer, "0123456789") != 0) return -5;
    close(fd);
    unlink(path);
    if (stat(path, &info) == 0 || *__errno() != 2 /* ENOENT */) return -6;
    return 0;
}

// errno survives the app clearing it, and a later error replaces it.
int test_errno(void) {
    *__errno() = 0;
    strtol("12", 0, 10);
    if (*__errno() != 0) return -1;
    strtod("1e999999", 0);
    if (*__errno() != 34 /* ERANGE */) return -2;
    *__errno() = 0;
    strtod("1e999999", 0);
    return *__errno() == 34 ? 0 : -3;
}

// --- Threads, with bionic's types -------------------------------------------------------

static struct { int words[10]; } mutex;  // PTHREAD_MUTEX_INITIALIZER: 40 zero bytes
static struct { int words[10]; } recursive = {{1 << 14}};  // PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
static struct { int words[12]; } condition;
static long counter;
static int once_control;
static int once_runs;
static void once_function(void) { once_runs++; }

static void* worker(void* argument) {
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&mutex);
        counter++;
        pthread_mutex_unlock(&mutex);
    }
    pthread_once(&once_control, once_function);
    return argument;
}

int test_threads(void) {
    long threads[4];
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&threads[i], 0, worker, (void*)(long)(i + 1)) != 0) return -1;
    }
    for (int i = 0; i < 4; i++) {
        void* result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != (void*)(long)(i + 1)) return -2;
    }
    if (counter != 400000 || once_runs != 1) return -3;
    if (pthread_mutex_lock(&recursive) != 0 || pthread_mutex_lock(&recursive) != 0) return -4;
    pthread_mutex_unlock(&recursive);
    pthread_mutex_unlock(&recursive);
    // A timed wait that expires must report Linux's ETIMEDOUT (110).
    long deadline[2];
    clock_gettime(0 /* CLOCK_REALTIME */, deadline);
    deadline[1] += 20000000;
    if (deadline[1] >= 1000000000) {
        deadline[0]++;
        deadline[1] -= 1000000000;
    }
    pthread_mutex_lock(&mutex);
    int result = pthread_cond_timedwait(&condition, &mutex, deadline);
    pthread_mutex_unlock(&mutex);
    return result == 110 ? 0 : -5;
}

// --- Callbacks from the C library into this code ------------------------------------------

static int compare_ints(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
int test_callback(void) {
    int values[] = {5, 3, 9, 1, 7};
    qsort(values, 5, sizeof(int), compare_ints);
    return values[0] == 1 && values[1] == 3 && values[4] == 9;
}
