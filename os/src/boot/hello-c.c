#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int malloc_test(void) {
    int* a = (int*)malloc(10 * sizeof(int));
    if (!a) { printf("FAIL: malloc returned NULL\n"); return 1; }
    for (int i = 0; i < 10; i++) a[i] = i * i;
    for (int i = 0; i < 10; i++) {
        if (a[i] != i * i) {
            printf("FAIL: a[%d] = %d, expected %d\n", i, a[i], i * i);
            free(a);
            return 1;
        }
    }
    free(a);

    int* p = (int*)calloc(20, sizeof(int));
    if (!p) { printf("FAIL: calloc returned NULL\n"); return 1; }
    for (int i = 0; i < 20; i++) {
        if (p[i] != 0) {
            printf("FAIL: calloc not zeroed at %d\n", i);
            free(p);
            return 1;
        }
    }
    for (int i = 0; i < 20; i++) p[i] = i;
    int* q = (int*)realloc(p, 40 * sizeof(int));
    if (!q) { printf("FAIL: realloc returned NULL\n"); free(p); return 1; }
    for (int i = 0; i < 20; i++) {
        if (q[i] != i) {
            printf("FAIL: realloc data lost at %d\n", i);
            free(q);
            return 1;
        }
    }
    free(q);

    printf("malloc_test: PASS\n");
    return 0;
}

static int printf_test(void) {
    printf("printf_test: string=%s, int=%d, hex=0x%x, char='%c'\n",
           "hello", 42, 0xDEAD, '!');

    char buf[64];
    int     n = snprintf(buf, sizeof(buf), "%s %d", "snprintf", 99);
    if (n != 11 || strcmp(buf, "snprintf 99") != 0) {
        printf("FAIL: snprintf returned %d, buf=\"%s\"\n", n, buf);
        return 1;
    }

    n = snprintf(buf, 6, "longstr%d", 123);
    if (n != 10) {
        printf("FAIL: snprintf truncated ret=%d (expected 10)\n", n);
        return 1;
    }
    if (strcmp(buf, "longs") != 0) {
        printf("FAIL: snprintf truncated buf=\"%s\"\n", buf);
        return 1;
    }

    printf("printf_test: PASS\n");
    return 0;
}

static void cleanup_msg(void) {
    write(STDOUT_FILENO, "atexit: cleanup called\n", 24);
}

static int atexit_test(void) {
    atexit(cleanup_msg);
    printf("atexit_test: registered cleanup, exiting...\n");
    return 0;
}

static int large_alloc_test(void) {
    size_t sizes[] = {16, 64, 256, 1024, 4096, 0};
    for (int i = 0; sizes[i]; i++) {
        void* p = malloc(sizes[i]);
        if (!p) {
            printf("FAIL: malloc(%lu) returned NULL\n", sizes[i]);
            return 1;
        }
        memset(p, 0xAB, sizes[i]);
        free(p);
    }
    printf("large_alloc_test: PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    printf("=== libc test ===\n");
    printf("argc=%d\n", argc);
    if (argc > 0) printf("argv[0]=%s\n", argv[0] ? argv[0] : "(null)");

    int fail = 0;
    fail |= printf_test();
    fail |= malloc_test();
    fail |= large_alloc_test();
    fail |= atexit_test();

    if (fail)
        printf("SOME TESTS FAILED\n");
    else
        printf("=== ALL TESTS PASSED ===\n");

    return fail;
}
