#include "kernel.h"
#include "hal.h"
#include "sched.h"
#include "syscall.h"
#include "process.h"
#include "hpet.h"
#include "apic.h"
#include "swap.h"
#include "vmm.h"
#include "pmm.h"
#include "tty.h"


#define UART_BASE 0x3F8
#define UART_RBR  (UART_BASE + 0)
#define UART_THR  (UART_BASE + 0)
#define UART_IER  (UART_BASE + 1)
#define UART_FCR  (UART_BASE + 2)
#define UART_LCR  (UART_BASE + 3)
#define UART_LSR  (UART_BASE + 5)
#define UART_IRQ  4

#define PIT_CMD  0x43
#define PIT_DATA 0x40

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define IDT_ENTRIES 256
#define GDT_ENTRIES 7

typedef struct {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed)) idtr_t;

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));
static idtr_t idtr;

static uint64_t gdt[GDT_ENTRIES] __attribute__((aligned(8)));
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;
static tss64_t tss __attribute__((aligned(16)));

static uint8_t ist_stack0[8192] __attribute__((aligned(16)));
static uint8_t user_stack0[16384] __attribute__((aligned(16)));

typedef struct {
    irq_handler_t handler;
    void* data;
} irq_reg_t;

static irq_reg_t irq_handlers[48];
static volatile uint64_t timer_ticks = 0;
static volatile uint32_t timer_hz = 1000;
int hal_smap_enabled(void) {
    static int available = 0;
    static int checked = 0;
    if (!checked) {
        checked = 1;
        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(7), "c"(0));
        available = (ebx & (1 << 20)) ? 1 : 0;
    }
    return available;
}

static int uart_rx_irq_active = 0;

static inline void io_wait(void) { outb(0x80, 0); }

int hal_cpu_has_mwait(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    return (ecx >> 3) & 1;
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

void hal_sti(void) { asm volatile("sti"); }
void hal_cli(void) { asm volatile("cli"); }

cpu_flags_t hal_save_irq(void) {
    cpu_flags_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void hal_restore_irq(cpu_flags_t flags) {
    if (flags & 0x200)
        asm volatile("sti" : : : "memory");
}

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    gdt[i] = (limit & 0xFFFF)
           | ((uint64_t)(base & 0xFFFFFF) << 16)
           | ((uint64_t)(access & 0xFF) << 40)
           | ((uint64_t)((limit >> 16) & 0x0F) << 48)
           | ((uint64_t)(flags & 0x0F) << 52)
           | ((uint64_t)((base >> 24) & 0xFF) << 56);
}

static void gdt_set_tss(int i, uint64_t tss_addr, uint32_t tss_size) {
    uint32_t base_low = tss_addr & 0xFFFFFFFF;
    uint32_t base_high = tss_addr >> 32;
    gdt[i] = (tss_size & 0xFFFF)
           | ((uint64_t)(base_low & 0xFFFFFF) << 16)
           | ((uint64_t)0x89 << 40)
           | ((uint64_t)((base_low >> 24) & 0xFF) << 56);
    gdt[i+1] = base_high;
}

static void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0, 0x9A, 0x02);
    gdt_set_entry(2, 0, 0, 0x92, 0x00);
    gdt_set_entry(3, 0, 0, 0xFA, 0x02);
    gdt_set_entry(4, 0, 0, 0xF2, 0x00);
    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)gdt;
    asm volatile("lgdt %0" : : "m"(gdtr));

    asm volatile("movw $0x10, %%ax; movw %%ax, %%ds; movw %%ax, %%es; movw %%ax, %%ss"
        : : : "ax");

    asm volatile("movw $0x28, %%ax; ltr %%ax" : : : "ax");
}

static void idt_set_gate(uint8_t vec, uint64_t handler, uint8_t dpl) {
    idt_entry_t* e = &idt[vec];
    e->offset_low  = handler & 0xFFFF;
    e->selector    = 0x08;
    e->ist         = (vec == 8) ? 1 : 0;
    e->type_attr   = 0x8E | (dpl << 5);
    e->offset_mid  = (handler >> 16) & 0xFFFF;
    e->offset_high = (handler >> 32) & 0xFFFFFFFF;
    e->reserved    = 0;
}

static void idt_init(void) {
    idtr.size = sizeof(idt) - 1;
    idtr.offset = (uint64_t)idt;

    for (int i = 0; i < 256; i++) {
        if (isr_vectors[i]) {
            idt_set_gate(i, isr_vectors[i], (i == 128) ? 3 : 0);
        }
    }

    asm volatile("lidt %0" : : "m"(idtr));

    kmemset(&tss, 0, sizeof(tss));
    tss.rsp[0] = (uint64_t)user_stack0 + sizeof(user_stack0);
    tss.ist[0] = (uint64_t)ist_stack0 + sizeof(ist_stack0);
    tss.ist[1] = (uint64_t)ist_stack0 + sizeof(ist_stack0);
}

void hal_set_kernel_stack(uint64_t rsp0) {
    tss.rsp[0] = rsp0;
}

uint64_t hal_get_kernel_stack(void) {
    return tss.rsp[0];
}

void hal_enable_irqs(void) {
    // Unmask timer IRQ0, UART IRQ4, ATA IRQ14+15
    uint16_t mask = inb(PIC1_DATA) | (inb(PIC2_DATA) << 8);
    mask &= ~(1 << 0);       // Timer IRQ0
    mask &= ~(1 << UART_IRQ); // UART IRQ4
    mask &= ~(1 << 14);       // ATA primary IRQ14
    mask &= ~(1 << 15);       // ATA secondary IRQ15
    outb(PIC1_DATA, mask & 0xFF);
    outb(PIC2_DATA, (mask >> 8) & 0xFF);
}

static void pic_remap(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFB); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
}

void hal_irq_eoi(uint8_t irq) {
    if (apic_present) apic_eoi();
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

err_t hal_irq_register(uint8_t irq, irq_handler_t handler, void* data) {
    if (irq >= 48) return ERR_INVAL;
    irq_handlers[irq].handler = handler;
    irq_handlers[irq].data = data;
    __sync_synchronize();
    // Don't unmask automatically; unmask via hal_enable_irqs()
    return ERR_OK;
}

err_t hal_timer_init(uint32_t hz) {
    if (hz == 0) hz = 1000;
    timer_hz = hz;
    uint32_t divisor = 1193182 / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_DATA, divisor & 0xFF);
    outb(PIT_DATA, (divisor >> 8) & 0xFF);
    return ERR_OK;
}

uint64_t hal_timer_get_ticks(void) { return timer_ticks; }

uint64_t hal_timer_get_ns(void) {
    if (hpet_present) {
        return hpet_ns();
    }
    unsigned __int128 ns = (unsigned __int128)timer_ticks * 1000000000ULL;
    return (uint64_t)(ns / timer_hz);
}

uint32_t hal_timer_get_hz(void) { return timer_hz; }

static void uart_rx_isr(int_frame_t* frame, void* data) {
    (void)frame; (void)data;
    while (inb(UART_LSR) & 1) {
        char c = inb(UART_RBR);
        tty_input_push(c);
    }
    need_reschedule = 1;
}

err_t hal_uart_rx_init(void) {
    err_t e = hal_irq_register(UART_IRQ, uart_rx_isr, NULL);
    if (e) return e;
    outb(UART_IER, 0x01);
    uart_rx_irq_active = 1;
    return ERR_OK;
}

char hal_uart_getchar(void) {
    if (!uart_rx_irq_active) {
        while (!(inb(UART_LSR) & 1));
        return inb(UART_RBR);
    }
    return tty_getchar();
}

int hal_uart_data_available(void) {
    if (uart_rx_irq_active) {
        extern tty_t tty_console;
        return (tty_console.raw_head != tty_console.raw_tail) ? 1 : 0;
    }
    return (inb(UART_LSR) & 1) ? 1 : 0;
}

static void uart_init(void) {
    outb(UART_IER, 0x00);
    outb(UART_LCR, 0x80);
    outb(UART_THR, 0x01);
    outb(UART_IER, 0x00);
    outb(UART_LCR, 0x03);
    outb(UART_FCR, 0x07);
    outb(UART_IER, 0x00);
}

static hal_mmap_entry_t mmap_entries[MAX_MMAP_ENTRIES];
static int mmap_entry_count = 0;

int hal_get_mmap_entries(hal_mmap_entry_t* out, int max) {
    int n = mmap_entry_count < max ? mmap_entry_count : max;
    for (int i = 0; i < n; i++) out[i] = mmap_entries[i];
    return mmap_entry_count;
}

uint64_t hal_get_mem_size(uint64_t mb_info) {
    mmap_entry_count = 0;

    if (mb_info == 0) {
        return 512 * 1024 * 1024;
    }

    multiboot_info_t* mbi = (multiboot_info_t*)(uint64_t)mb_info;
    uint64_t max_addr = 0;

    if (mbi->flags & (1 << 6)) {
        mmap_entry_t* entry = (mmap_entry_t*)(uint64_t)mbi->mmap_addr;
        uint32_t remaining = mbi->mmap_length;
        while (remaining > 0 && mmap_entry_count < MAX_MMAP_ENTRIES) {
            mmap_entries[mmap_entry_count].start = entry->base_addr;
            mmap_entries[mmap_entry_count].end = entry->base_addr + entry->length;
            mmap_entries[mmap_entry_count].type = entry->type;
            mmap_entry_count++;

            if (entry->type == 1) {
                uint64_t end = entry->base_addr + entry->length;
                if (end > max_addr) max_addr = end;
            }
            uint32_t entry_size;
            if (entry->size > ((uint32_t)-1) - 4) break;
            entry_size = entry->size + 4;
            entry = (mmap_entry_t*)((uint64_t)entry + entry_size);
            remaining -= (remaining >= entry_size) ? entry_size : remaining;
        }
    } else if (mbi->flags & (1 << 0)) {
        max_addr = (uint64_t)mbi->mem_upper * 1024;
    }

    if (max_addr < 32 * 1024 * 1024) max_addr = 32 * 1024 * 1024;
    return max_addr;
}

void interrupt_handler(int_frame_t* frame) {
    uint8_t vec = frame->vector & 0xFF;

    if (vec == 32) {
        timer_ticks++;
    }

    if (vec >= 32 && vec < 80) {
        uint8_t irq = vec - 32;
        if (irq_handlers[irq].handler) {
            irq_handlers[irq].handler(frame, irq_handlers[irq].data);
        }
        hal_irq_eoi(irq);
        /* Deliver pending signals when returning to user mode */
        if ((frame->cs & 3) == 3 && current_thread && current_thread->proc) {
            signal_deliver_custom(current_thread->proc, frame);
        }
        return;
    }

    if (vec == 128) {
        syscall_handler(frame);
        /* Deliver pending custom signals on return to user mode */
        if ((frame->cs & 3) == 3 && current_thread && current_thread->proc) {
            signal_deliver_custom(current_thread->proc, frame);
        }
        return;
    }

    if (vec == 14) {
        asm volatile("cli");
        uint64_t cr2 = read_cr2();
        uint64_t fault_rip = frame->rip;
        uint64_t err = frame->error_code;
        if ((frame->cs & 3) == 3) {
            /* Check if this is a swapped-out page */
            if (current_thread && current_thread->proc) {
                uint64_t cr3 = current_thread->cr3;
                if (cr3) {
                    page_entry_t* pte = vmm_walk_pagetable(cr3, cr2);
                    if (pte && (*pte & SWAP_PTE_MARKER) && !(*pte & PAGE_PRESENT)) {
                        int slot = swap_decode_pte(*pte);
                        kprintf("[SWAP] Page fault resolved: slot %d -> %llx\n",
                                slot, cr2);
                        uint64_t new_page = pmm_alloc_page();
                        if (!new_page) {
                            kprintf("[SWAP] Failed to allocate page for swap-in\n");
                            process_exit(current_thread->proc, -11);
                            thread_exit(-11);
                            for (;;) asm volatile("cli; hlt");
                        }
                        if (swap_in(slot, new_page) != ERR_OK) {
                            kprintf("[SWAP] swap_in failed for slot %d\n", slot);
                        }
                        swap_free_slot(slot);
                        *pte = new_page | PAGE_PRESENT | swap_decode_pte_flags(*pte);
                        vmm_flush_tlb_page(cr2);
                        return;
                    }
                }
            }
            kprintf("PAGE FAULT pid=%lu rip=%lx addr=%lx error=%lu -- killing process\n",
                    current_thread && current_thread->proc ? current_thread->proc->pid : 0,
                    fault_rip, cr2, err);
            if (current_thread && current_thread->proc) {
                process_exit(current_thread->proc, -11);
                thread_exit(-11);
            }
            for (;;) { asm volatile("cli; hlt"); }
        } else {
            kprintf("PAGE FAULT at %lx, rip=%lx, error=%lu (KERNEL)\n",
                    cr2, fault_rip, err);
            kpanic("Page fault (kernel mode)");
        }
    }

    if (vec == 13) {
        kprintf("GP FAULT rip=%lx error=%lu\n", frame->rip, frame->error_code);
        kpanic("General protection fault");
    }

    if (vec == 8) {
        kprintf("DOUBLE FAULT rip=%lx\n", frame->rip);
        for (;;) { asm volatile("cli; hlt"); }
    }

    kprintf("UNHANDLED INTERRUPT vec=%lu rip=%lx\n", (uint64_t)vec, frame->rip);
    kpanic("Unhandled interrupt");
}


void hal_poweroff(void) {
    kputs("System poweroff.\n");
    outw(0xB004, 0x2000);
    outw(0x6004, 0x2000);
    asm volatile("outw %%ax, %%dx" : : "d"((uint16_t)0x604), "a"((uint16_t)0x2000));
    for (;;) { asm volatile("cli; hlt"); }
}

void hal_reboot(void) {
    kputs("System reboot.\n");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) asm("pause");
    outb(0x64, 0xFE);
    for (;;) { asm volatile("cli; hlt"); }
}

err_t hal_init(uint64_t mb_info_phys) {
    kmemset(idt, 0, sizeof(idt));
    kmemset(gdt, 0, sizeof(gdt));
    kmemset(irq_handlers, 0, sizeof(irq_handlers));

    gdt_init();
    idt_init();
    pic_remap();
    uart_init();
    hal_timer_init(1000);

    // Enable SMEP and SMAP if supported
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0));

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (ebx & (1 << 7)) { // SMEP is bit 7 of EBX when CPUID=7, ECX=0
        cr4 |= (1UL << 20);
    }
    if (hal_smap_enabled()) {
        cr4 |= (1UL << 21);
    }
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    kputs("[HAL] Layer 1 initialized: GDT, IDT, PIC, UART, Timer\n");
    return ERR_OK;
}
