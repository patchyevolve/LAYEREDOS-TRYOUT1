#include "kernel.h"

/* Stack canary — arbitrary but non-zero to detect memset-to-zero overflows */
__attribute__((used)) uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABE;

__attribute__((used)) void __stack_chk_fail(void) {
    kprintf("[PANIC] Kernel stack smashing detected!\n");
    for (;;)
        ;
}
