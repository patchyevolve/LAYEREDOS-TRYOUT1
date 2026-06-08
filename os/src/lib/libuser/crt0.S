.section .text
.global _start
.type _start, @function
_start:
    movq %rdi, %r12
    movq %rsi, %r13
    movq %rdx, %r14
    xorq %rbp, %rbp

    movq %rsp, %rdi
    call __libc_init

    movq %r12, %rdi
    movq %r13, %rsi
    movq %r14, %rdx
    call main

    movq %rax, %rdi
    call exit

    movq $0, %rax
    movq $0, %rdi
    int $0x80
    hlt

/* __libc_init is now in stdlib.c */
