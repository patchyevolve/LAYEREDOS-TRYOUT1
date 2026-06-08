/* libdyn.so — test shared library for dynamic linking */

extern int printf(const char* fmt, ...);

int dyn_hello(void) {
    return 42;
}

int dyn_greet(const char* name) {
    return printf("Hello from libdyn, %s!\n", name);
}

int dyn_double(int x) {
    return x * 2;
}
