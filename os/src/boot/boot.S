.section .note.pvh, "a", @note
.align 4
.long 2f - 1f
.long 4f - 3f
.long 18
1: .asciz "Xen"
2: .align 4
3: .long start
4: .align 4

.section .boot.data, "aw", @progbits
.align 4096
pml4_table:    .space 4096, 0
pdpt_table:    .space 4096, 0
pd_table:      .space 4096, 0
gdt_table:
.quad 0x0000000000000000
.quad 0x0020980000000000
.quad 0x0000920000000000
gdt_end:

gdt_descriptor:
.word gdt_end - gdt_table - 1
.long gdt_table

.align 8
magic_saved:    .quad 0
mb_info_saved:  .quad 0

.equ KERNEL_VMA_BASE, 0xFFFFFFFFC0000000
.equ PML4_ADDR,   0x7000
.equ PDPT_ADDR,   0x8000
.equ PD_ADDR,     0x9000


.section .bss, "aw", @nobits
.align 16
stack_bottom:
.space 32768
stack_top:

.section .boot.text, "ax", @progbits
.globl start
.type start, @function
.code32
start:
    /* Initialize UART first! */
    movw $0x3F8 + 1, %dx       /* IER: disable interrupts */
    xorb %al, %al
    outb %al, %dx

    movw $0x3F8 + 3, %dx       /* LCR: enable DLAB */
    movb $0x80, %al
    outb %al, %dx

    movw $0x3F8 + 0, %dx       /* DLL: divisor 1 */
    movb $0x01, %al
    outb %al, %dx
    movw $0x3F8 + 1, %dx       /* DLM: 0 */
    xorb %al, %al
    outb %al, %dx

    movw $0x3F8 + 3, %dx       /* LCR: 8 bits, no parity, 1 stop bit */
    movb $0x03, %al
    outb %al, %dx

    movw $0x3F8 + 2, %dx       /* FCR: disable FIFO */
    movb $0x00, %al
    outb %al, %dx

    movw $0x3F8 + 4, %dx       /* MCR: DTR/RTS */
    movb $0x03, %al
    outb %al, %dx

    /* Now output "BOOTING" to serial */
    movw $0x3F8, %dx
    movb $'B', %al
    call uart_putb
    movb $'O', %al
    call uart_putb
    movb $'O', %al
    call uart_putb
    movb $'T', %al
    call uart_putb
    movb $'I', %al
    call uart_putb
    movb $'N', %al
    call uart_putb
    movb $'G', %al
    call uart_putb
    movb $'\n', %al
    call uart_putb

    movl %eax, magic_saved - KERNEL_VMA_BASE
    movl %ebx, mb_info_saved - KERNEL_VMA_BASE
    cli

    leal (stack_top - KERNEL_VMA_BASE), %esp

    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    cmpl %ecx, %eax
    je .no_cpuid

    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .no_long_mode

    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .no_long_mode

    movl $PML4_ADDR, %edi
    movl $0x3000, %ecx
    xorl %eax, %eax
    cld
    rep stosb

    movl $(PDPT_ADDR | 0x03), (PML4_ADDR)        /* lower half identity map */
    movl $(PDPT_ADDR | 0x03), (PML4_ADDR + 511*8) /* higher half for kernel */

    movl $(PD_ADDR | 0x03), (PDPT_ADDR)            /* identity: 0-1GB → PD */
    movl $(PD_ADDR | 0x03), (PDPT_ADDR + 511*8)    /* higher half: last 1GB of VA → PD */

    movl $PD_ADDR, %edi
    movl $0x00000083, %eax
    movl $256, %ecx
1:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $8, %edi
    addl $0x200000, %eax
    subl $1, %ecx
    jnz 1b

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl $PML4_ADDR, %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    rdmsr
    orl $((1 << 8) | (1 << 11)), %eax
    wrmsr

    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    lgdt gdt_descriptor

    ljmp $0x08, $1f

.code64
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Clear BSS — on a warm reset (panic→reboot) QEMU preserves physical RAM
     * and the multiboot loader's BSS-clearing does not re-run.  Stale pointers
     * in BSS (free_list, all_threads_head, run queues, etc.) cause crashes.
     *
     * We use the page-table area in .boot.data as a temporary stack — those
     * tables are no longer needed after switching to long mode. */
    movabsq $pml4_table, %rbx
    addq    $16384, %rbx            /* end of page-table region (12 KB) + padding */
    movq    %rbx, %rsp

    movabsq $_bss_start, %rdi
    movabsq $_bss_end, %rcx
    subq    %rdi, %rcx
    xorl    %eax, %eax
    cld
    rep     stosb

    movabsq $stack_top, %rsp

    movq magic_saved, %rdi
    movq mb_info_saved, %rsi
    movabsq $kmain, %rax
    call *%rax

2:
    cli
    hlt
    jmp 2b

.code32
uart_putb:
    push %edx
    push %ax
    movw $0x3F8 + 5, %dx  /* UART_LSR */
1:
    inb %dx, %al
    testb $0x20, %al        /* Check TX Empty */
    jz 1b
    pop %ax
    movw $0x3F8, %dx
    outb %al, %dx
    pop %edx
    ret

.no_cpuid:
    movw $0x3F8, %dx
1:  movw $0x45, %ax
    outb %al, %dx
    movw $0x72, %ax
    outb %al, %dx
    movw $0x72, %ax
    outb %al, %dx
    jmp 2b

.no_long_mode:
    movw $0x3F8, %dx
    movw $0x4E, %ax
    outb %al, %dx
    movw $0x4F, %ax
    outb %al, %dx
    jmp 2b
