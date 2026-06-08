#include "syscall_defs.h"

.section .text
.global _start
.type _start, @function
_start:
    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea msg(%rip), %rsi
    movq $23, %rdx
    int $0x80

    /* Test SYS_CLONE: clone(CLONE_VM, child_stack) */
    lea child_stack_top(%rip), %rsi
    movq $SYS_CLONE, %rax
    movq $1, %rdi
    int $0x80

    cmpq $0, %rax
    jnz parent_done

    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea child_msg(%rip), %rsi
    movq $6, %rdx
    int $0x80

    movq $SYS_EXIT, %rax
    xorq %rdi, %rdi
    int $0x80

parent_done:
    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea parent_msg(%rip), %rsi
    movq $8, %rdx
    int $0x80

    movq $SYS_EXIT, %rax
    xorq %rdi, %rdi
    int $0x80

.section .data
msg: .ascii "Hello from user mode!\n"
child_msg: .ascii "child\n"
parent_msg: .ascii "parent\n"
.section .bss
child_stack: .skip 4096
child_stack_top:
