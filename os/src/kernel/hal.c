#include "kernel.h"
#include "hal.h"

#define UART_BASE 0x3F8
#define UART_RBR  (UART_BASE + 0)
#define UART_THR  (UART_BASE + 0)
#define UART_IER  (UART_BASE + 1)
#define UART_FCR  (UART_BASE + 2)
#define UART_LCR  (UART_BASE + 3)
#define UART_LSR  (UART_BASE + 5)

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

typedef struct {
    irq_handler_t handler;
    void* data;
} irq_reg_t;

static irq_reg_t irq_handlers[48];
static volatile uint64_t timer_ticks = 0;
static volatile uint32_t timer_hz = 1000;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void) { outb(0x80, 0); }

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
    asm volatile("pushfq; popq %0" : "=r"(flags));
    asm volatile("cli");
    return flags;
}

void hal_restore_irq(cpu_flags_t flags) {
    if (flags & 0x200) asm volatile("sti");
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

err_t hal_idt_set_gate(int vec, void* handler, uint8_t dpl) {
    if (vec < 0 || vec >= (int)IDT_ENTRIES) return ERR_INVAL;
    idt_set_gate(vec, (uint64_t)handler, dpl);
    return ERR_OK;
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
    tss.ist[0] = (uint64_t)ist_stack0 + sizeof(ist_stack0);
    tss.ist[1] = (uint64_t)ist_stack0 + sizeof(ist_stack0);
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
    outb(PIC1_DATA, 0xFD); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
}

void hal_irq_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

err_t hal_irq_register(uint8_t irq, irq_handler_t handler, void* data) {
    if (irq >= 48) return ERR_INVAL;
    irq_handlers[irq].handler = handler;
    irq_handlers[irq].data = data;
    if (irq < 16) {
        uint16_t mask = inb(PIC1_DATA) | (inb(PIC2_DATA) << 8);
        mask &= ~(1 << irq);
        outb(PIC1_DATA, mask & 0xFF);
        outb(PIC2_DATA, (mask >> 8) & 0xFF);
    }
    return ERR_OK;
}

err_t hal_irq_unregister(uint8_t irq) {
    if (irq >= 48) return ERR_INVAL;
    irq_handlers[irq].handler = NULL;
    irq_handlers[irq].data = NULL;
    if (irq < 16) {
        uint16_t mask = inb(PIC1_DATA) | (inb(PIC2_DATA) << 8);
        mask |= (1 << irq);
        outb(PIC1_DATA, mask & 0xFF);
        outb(PIC2_DATA, (mask >> 8) & 0xFF);
    }
    return ERR_OK;
}

err_t hal_timer_init(uint32_t hz) {
    timer_hz = hz;
    uint32_t divisor = 1193182 / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_DATA, divisor & 0xFF);
    outb(PIT_DATA, (divisor >> 8) & 0xFF);
    return ERR_OK;
}

uint64_t hal_timer_get_ticks(void) { return timer_ticks; }

uint64_t hal_timer_get_ns(void) {
    return (timer_ticks * 1000000000ULL) / timer_hz;
}

void hal_uart_putchar(char c) {
    while (!(inb(UART_LSR) & 0x20));
    outb(UART_THR, c);
    if (c == '\n') {
        while (!(inb(UART_LSR) & 0x20));
        outb(UART_THR, '\r');
    }
}

char hal_uart_getchar(void) {
    while (!(inb(UART_LSR) & 1));
    return inb(UART_RBR);
}

int hal_uart_data_available(void) {
    return (inb(UART_LSR) & 1) ? 1 : 0;
}

static void uart_init(void) {
    outb(UART_IER, 0x00);
    outb(UART_LCR, 0x80);
    outb(UART_THR, 0x01);
    outb(UART_IER, 0x00);
    outb(UART_LCR, 0x03);
    outb(UART_FCR, 0x07);
    outb(UART_IER, 0x01);
}

uint64_t hal_get_mem_size(uint64_t mb_info) {
    typedef struct {
        uint32_t size;
        uint64_t base_addr;
        uint64_t length;
        uint32_t type;
    } __attribute__((packed)) mmap_entry_t;
    typedef struct {
        uint32_t flags;
        uint32_t mem_lower;
        uint32_t mem_upper;
        uint32_t boot_device;
        uint32_t cmdline;
        uint32_t mods_count;
        uint32_t mods_addr;
        uint32_t syms[4];
        uint32_t mmap_length;
        uint32_t mmap_addr;
        uint32_t drives_length;
        uint32_t drives_addr;
    } __attribute__((packed)) multiboot_info_t;

    if (mb_info == 0) {
        return 512 * 1024 * 1024;
    }

    multiboot_info_t* mbi = (multiboot_info_t*)(uint64_t)mb_info;
    uint64_t max_addr = 0;

    if (mbi->flags & (1 << 6)) {
        mmap_entry_t* entry = (mmap_entry_t*)(uint64_t)mbi->mmap_addr;
        uint32_t remaining = mbi->mmap_length;
        while (remaining > 0) {
            if (entry->type == 1) {
                uint64_t end = entry->base_addr + entry->length;
                if (end > max_addr) max_addr = end;
            }
            uint32_t entry_size = entry->size + 4;
            entry = (mmap_entry_t*)((uint64_t)entry + entry_size);
            remaining -= entry_size;
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
        return;
    }

    if (vec == 128) return;

    if (vec == 14) {
        uint64_t cr2 = read_cr2();
        kprintf("PAGE FAULT at 0x%lx, rip=0x%lx, error=%lu\n",
                cr2, frame->rip, frame->error_code);
        kpanic("Page fault");
    }

    if (vec == 13) {
        kprintf("GP FAULT rip=0x%lx error=%lu\n", frame->rip, frame->error_code);
        kpanic("General protection fault");
    }

    if (vec == 8) {
        kprintf("DOUBLE FAULT rip=0x%lx\n", frame->rip);
        for (;;) { asm volatile("cli; hlt"); }
    }

    kprintf("UNHANDLED INTERRUPT vec=%lu rip=0x%lx\n", (uint64_t)vec, frame->rip);
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
    while (inb(0x64) & 2);
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

    kputs("[HAL] Layer 1 initialized: GDT, IDT, PIC, UART, Timer\n");
    return ERR_OK;
}
