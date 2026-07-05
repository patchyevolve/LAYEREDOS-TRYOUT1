#include <stdint.h>

/* Userspace stack canary */
__attribute__((used)) uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABE;

__attribute__((used)) void __stack_chk_fail(void) {
    /* Halt on stack smashing; in a real system we'd write to stderr and _exit */
    for (;;)
        ;
}
