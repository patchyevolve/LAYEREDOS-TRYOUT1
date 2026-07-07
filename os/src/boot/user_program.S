#include "syscall_defs.h"

.section .text
.global _start
.type _start, @function
_start:
    /* Infinite loop — test if user mode is even reached */
1:  jmp 1b
