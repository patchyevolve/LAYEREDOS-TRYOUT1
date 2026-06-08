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
    movq %rsp, (%rdi)

    movq (%rsi), %rsi
    movq (%rsi), %rsp

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
    /* r14 = arg, r15 = func — set by thread_create stack layout */
    testq %r15, %r15
    jz .Lnull_func_panic
    movq %r14, %rdi
    call *%r15
    movq %rax, %rdi
    call thread_exit
    int $0x20
.Lnull_func_panic:
    movabsq $null_func_msg, %rdi
    call kpanic


.global user_thread_entry
.type user_thread_entry, @function
user_thread_entry:
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

    movw $0x3FD, %dx
1:  inb %dx, %al
    testb $0x20, %al
    jz 1b
    movw $0x3F8, %dx
    movb $'[', %al
    outb %al, %dx
    movw $0x3FD, %dx
2:  inb %dx, %al
    testb $0x20, %al
    jz 2b
    movw $0x3F8, %dx
    movb $'U', %al
    outb %al, %dx
    movw $0x3FD, %dx
3:  inb %dx, %al
    testb $0x20, %al
    jz 3b
    movw $0x3F8, %dx
    movb $']', %al
    outb %al, %dx
    movw $0x3FD, %dx
4:  inb %dx, %al
    testb $0x20, %al
    jz 4b
    movw $0x3F8, %dx
    movb $'\n', %al
    outb %al, %dx

    iretq

.global fork_child_entry
.type fork_child_entry, @function
fork_child_entry:
    /* The child process of fork() starts here.
     * Stack layout (set up by sys_fork):
     *   [rsp+0]  = rax = 0  (return value for child)
     *   [rsp+8]  = rbx
     *   [rsp+16] = rcx
     *   [rsp+24] = rdx
     *   [rsp+32] = rsi
     *   [rsp+40] = rdi
     *   [rsp+48] = rbp
     *   [rsp+56] = r8
     *   [rsp+64] = r9
     *   [rsp+72] = r10
     *   [rsp+80] = r11
     *   [rsp+88] = r12
     *   [rsp+96] = r13
     *   [rsp+104] = r14
     *   [rsp+112] = r15
     *   [rsp+120] = vector = 128
     *   [rsp+128] = error_code = 0
     *   [rsp+136] = user RIP
     *   [rsp+144] = user CS
     *   [rsp+152] = user RFLAGS
     *   [rsp+160] = user RSP
     *   [rsp+168] = user SS
     */
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

    addq $16, %rsp      /* skip vector and error_code */
    iretq

.global signal_trampoline
.type signal_trampoline, @function
signal_trampoline:
    /* This is invoked by the user signal handler's return.
     * rsp points to a sigframe_t on the user stack.
     * We call sigreturn syscall (syscall number 27) to restore
     * the original context and return to the interrupted code.
     */
    movq %rsp, %rdi
    movq $27, %rax
    int $0x80
    /* If sigreturn fails, just loop */
1:  hlt
    jmp 1b

.section .rodata
null_func_msg:
    .asciz "thread_trampoline: NULL function pointer"
