.section .text

.macro ISR_NOERR num
.align 8
.global isr\num
.type isr\num, @function
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common_handler
.endm

.macro ISR_ERR num
.align 8
.global isr\num
.type isr\num, @function
isr\num:
    pushq $\num
    jmp isr_common_handler
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

ISR_NOERR 128

.extern interrupt_handler
.extern need_reschedule
.extern schedule
.type isr_common_handler, @function
isr_common_handler:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    movq %rsp, %rdi
    cld
    call interrupt_handler

    cmpl $0, need_reschedule(%rip)
    je 1f
    call schedule
1:

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    addq $16, %rsp
    iretq

.section .rodata
.global isr_vectors
.type isr_vectors, @object
isr_vectors:
.quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
.quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
.quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
.quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.quad isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
.quad isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
.zero 8 * 80
.quad isr128
