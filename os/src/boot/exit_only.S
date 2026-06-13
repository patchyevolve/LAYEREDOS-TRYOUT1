#include "syscall_defs.h"

.section .text
.global _start
.type _start, @function
_start:
    movq $SYS_EXIT, %rax
    xorq %rdi, %rdi
    int $0x80
