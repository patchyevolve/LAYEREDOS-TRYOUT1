#include "syscall_defs.h"

.section .text
.global _start
.type _start, @function
_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call main
    movq %rax, %rdi
    movq $SYS_EXIT, %rax
    int $0x80
