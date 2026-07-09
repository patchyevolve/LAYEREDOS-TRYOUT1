/* AP trampoline — runs at physical address 0x4000 (SIPI vector page 4).
 * 16-bit real-mode entry -> protected mode -> 64-bit long mode.
 * Jumps to ap_entry(percpu_ptr).
 *
 * The BSP copies this blob (from _trampoline_start to _trampoline_end)
 * to physical address 0x4000 and fills in fields at offsets 0x200-0x21F.
 *
 * Layout:
 *   offset  content
 *   0x000   16-bit real-mode entry
 *   0x100   GDT (4 descriptors + gdt_desc = 38 bytes)
 *   0x130   32-bit protected-mode transition code
 *   0x200   BSP-filled fields: stack_ptr, percpu_ptr, kernel_cr3
 *   0x300   64-bit entry code
 *   0x400   End
 */
#define TRAMPOLINE_BASE 0x4000

.section .trampoline, "awx", @progbits

/* ===== offset 0x000: 16-bit real-mode entry ===== */
.org 0x000
.global _trampoline_start
_trampoline_start:

.code16
real_entry:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* Enable A20 gate */
    inb     $0x92, %al
    orb     $2, %al
    outb    %al, $0x92

    /* Load GDT: LGDT with 32-bit addr (0x67 addr-size prefix).
     * gdt_desc at page offset 0x120 → phys 0x4120
     */
    .byte   0x67, 0x0F, 0x01, 0x15
    .long   0x4120

    /* Enter protected mode */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    /* Far jump to 32-bit code.
     * pm_entry at page offset 0x130 → phys 0x4130
     */
    .byte   0x66, 0xEA
    .long   0x4130
    .word   0x08

/* ===== offset 0x100: GDT ===== */
.org 0x100
gdt_start:
    .quad 0x0000000000000000        /* 0x00: Null descriptor */
    .quad 0x00CF9A000000FFFF        /* 0x08: 32-bit code, ring 0 */
    .quad 0x00CF92000000FFFF        /* 0x10: 32-bit data, ring 0 */
    .quad 0x00AF9A000000FFFF        /* 0x18: 64-bit code, ring 0 */
gdt_end:

gdt_desc:
    .word gdt_end - gdt_start - 1   /* limit = 31 */
    .long 0x4100                    /* base = 0x4000 + 0x100 = 0x4100 */

/* ===== offset 0x130: protected mode code ===== */
.org 0x130
.code32
pm_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* Enable PAE */
    movl    %cr4, %eax
    orl     $0x20, %eax
    movl    %eax, %cr4

    /* Load CR3 from BSP-filled field at phys 0x4210 (32-bit abs addr, no prefix) */
    .byte   0xA1
    .long   0x4210
    movl    %eax, %cr3

    /* Enable Long Mode (EFER.LME = 1) + NXE (bit 11) */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x900, %eax
    wrmsr

    /* Enable paging */
    movl    %cr0, %eax
    orl     $0x80000001, %eax
    movl    %eax, %cr0

    /* Far jump to 64-bit code.
     * lm_entry at page offset 0x300 → phys 0x4300
     */
    .byte   0xEA
    .long   0x4300
    .word   0x18

/* ===== offset 0x200: BSP-filled data fields ===== */
.org 0x200
stack_ptr:
    .quad 0
.org 0x208
percpu_ptr:
    .quad 0
.org 0x210
kernel_cr3:
    .quad 0

/* ===== offset 0x300: 64-bit entry code ===== */
.org 0x300
.code64
lm_entry:
    /* Load stack pointer from phys 0x4200 */
    movq    $0x4200, %rax
    movq    (%rax), %rsp

    /* Load per-CPU data pointer from phys 0x4208 */
    movq    $0x4208, %rax
    movq    (%rax), %rdi

    /* Zero frame pointer */
    xorq    %rbp, %rbp

    /* Call ap_entry(percpu_ptr) */
    movabsq $ap_entry, %rax
    call    *%rax

    /* Never return */
1:  hlt
    jmp     1b

/* ===== end marker ===== */
.org 0x400
.global _trampoline_end
_trampoline_end:
