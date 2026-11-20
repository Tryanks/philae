// Loads the converted Android-ABI test library in an iOS process and checks what its
// functions return. Runs in the simulator: tests/native/run.sh.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void* library = dlopen(argv[1], RTLD_NOW);
    if (library == NULL) {
        printf("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
#define FUNCTION(type, name)                                   \
    __typeof__(type) name = (__typeof__(type))dlsym(library, #name); \
    if (name == NULL) {                                         \
        printf("FAIL no symbol " #name "\n");                   \
        return 1;                                               \
    }
    FUNCTION(int (*)(void), test_initializer)
    FUNCTION(const char* (*)(int), test_data_pointer)
    FUNCTION(int (*)(void), test_import_pointer)
    FUNCTION(int (*)(void), test_global)
    FUNCTION(int (*)(const char*), test_stack_guard)
    FUNCTION(int (*)(char*, size_t), test_format)
    FUNCTION(int (*)(char*, size_t), test_format_many)
    FUNCTION(int (*)(char*, size_t), test_va_list)
    FUNCTION(int (*)(void), test_scan)
    FUNCTION(int (*)(void), test_log)
    FUNCTION(int (*)(const char*), test_file)
    FUNCTION(int (*)(void), test_errno)
    FUNCTION(int (*)(void), test_threads)
    FUNCTION(int (*)(void), test_callback)
    int* exported_counter = dlsym(library, "exported_counter");

    check(test_initializer() == 42, "initializer array ran");
    check(strcmp(test_data_pointer(2), "two") == 0, "pointers in data are relocated");
    check(test_import_pointer() == 1, "function pointer to an imported function");
    check(test_global() == 8 && exported_counter != NULL && *exported_counter == 8, "exported variable");
    check(test_stack_guard("stack protector") == 15, "function with a stack protector");

    char text[256];
    int n = test_format(text, sizeof text);
    const char* expected = "str|-42| 3.14|1099511627776|x|beef|7   |00042|abc|%";
    check(n == (int)strlen(expected) && strcmp(text, expected) == 0, "snprintf with mixed arguments");
    if (strcmp(text, expected) != 0) printf("     got: %s\n", text);
    test_format_many(text, sizeof text);
    expected = "1 2 3 4 5 6 7 8 9 10 1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5 9.5 10.5";
    check(strcmp(text, expected) == 0, "snprintf with arguments on the stack");
    if (strcmp(text, expected) != 0) printf("     got: %s\n", text);
    test_va_list(text, sizeof text);
    check(strcmp(text, "va-9-0.25") == 0, "vsnprintf with Android's va_list");
    check(test_scan() == 1, "sscanf");
    check(test_log() == 1, "__android_log_print");
    int file = test_file(argv[2]);
    check(file == 0, "open/write/stat/read/unlink with Linux flags and struct stat");
    if (file != 0) printf("     step %d\n", file);
    int error = test_errno();
    check(error == 0, "errno in Linux numbering");
    if (error != 0) printf("     step %d\n", error);
    int threads = test_threads();
    check(threads == 0, "threads, mutexes, once, timed wait");
    if (threads != 0) printf("     step %d\n", threads);
    check(test_callback() == 1, "qsort calling back into Android code");

    printf(failures == 0 ? "ALL OK\n" : "%d FAILED\n", failures);
    return failures != 0;
}
