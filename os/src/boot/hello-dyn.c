/* hello-dyn.c — test program that uses a shared library */

extern int printf(const char* fmt, ...);
extern int puts(const char* s);
extern int dyn_hello(void);
extern int dyn_greet(const char* name);
extern int dyn_double(int x);

int main(int argc, char** argv, char** envp) {
    printf("hello-dyn: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    int v = dyn_hello();
    printf("dyn_hello() = %d (expected 42)\n", v);
    if (v != 42) { puts("FAIL: dyn_hello"); return 1; }

    v = dyn_double(21);
    printf("dyn_double(21) = %d (expected 42)\n", v);
    if (v != 42) { puts("FAIL: dyn_double"); return 1; }

    dyn_greet("world");

    puts("=== hello-dyn ALL TESTS PASSED ===");

    return 0;
}
