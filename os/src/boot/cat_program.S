#include "syscall_defs.h"

.section .text
.global _start
.type _start, @function

_start:
    movq $SYS_OPEN, %rax
    lea filename(%rip), %rdi
    xorq %rsi, %rsi
    int $0x80
    movq %rax, %r12

    cmpq $0, %r12
    jl .Lerr

    movq $SYS_READFILE, %rax
    movq %r12, %rdi
    lea buf(%rip), %rsi
    movq $128, %rdx
    int $0x80
    movq %rax, %r13

    movq $SYS_CLOSE, %rax
    movq %r12, %rdi
    int $0x80

    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea buf(%rip), %rsi
    movq %r13, %rdx
    int $0x80

    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea newline(%rip), %rsi
    movq $1, %rdx
    int $0x80

    movq $SYS_EXIT, %rax
    xorq %rdi, %rdi
    int $0x80

.Lerr:
    movq $SYS_WRITE, %rax
    movq $1, %rdi
    lea errmsg(%rip), %rsi
    movq $7, %rdx
    int $0x80

    movq $SYS_EXIT, %rax
    movq $1, %rdi
    int $0x80

.section .rodata
filename: .asciz "version.txt"
errmsg:   .asciz "ERROR!\n"
newline:  .byte 10

.section .bss
buf: .space 128
