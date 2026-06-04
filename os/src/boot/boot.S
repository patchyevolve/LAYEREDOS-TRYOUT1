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

    movl $(PDPT_ADDR | 0x03), (PML4_ADDR)
    movl $(PDPT_ADDR | 0x03), (PML4_ADDR + 511*8)

    movl $(PD_ADDR | 0x03), (PDPT_ADDR)
    movl $(PD_ADDR | 0x03), (PDPT_ADDR + 8)
    movl $(PD_ADDR | 0x03), (PDPT_ADDR + 511*8)
    movl $(PD_ADDR | 0x03), (PDPT_ADDR + 510*8)

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
    orl $(1 << 8), %eax
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

    movabsq $stack_top, %rsp

    movq magic_saved, %rdi
    movq mb_info_saved, %rsi
    movabsq $kmain, %rax
    call *%rax

2:
    cli
    hlt
    jmp 2b

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
