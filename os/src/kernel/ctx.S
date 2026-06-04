.section .text

.global switch_context
.type switch_context, @function
switch_context:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp

    movq (%rdi), %rdi
    movq %rsp, 8(%rdi)

    movq (%rsi), %rsi
    movq 8(%rsi), %rsp

    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    ret

.global thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    movq %r14, %rdi
    call *%r15
    movq %rax, %rdi
    call thread_exit
    int $0x20

.global schedule_trampoline
.type schedule_trampoline, @function
schedule_trampoline:
    call schedule
    ret

.global hal_get_rsp
.type hal_get_rsp, @function
hal_get_rsp:
    movq %rsp, %rax
    retq
