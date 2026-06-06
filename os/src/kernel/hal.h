#ifndef HAL_H
#define HAL_H

#include "types.h"

#define USER_CS 0x1B
#define USER_DS 0x23

typedef struct int_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} int_frame_t;

typedef void (*irq_handler_t)(int_frame_t* frame, void* data);

int   hal_cpu_has_mwait(void);
err_t hal_init(uint64_t mb_info_phys);
err_t hal_irq_register(uint8_t irq, irq_handler_t handler, void* data);
void  hal_irq_eoi(uint8_t irq);
void  hal_sti(void);
void  hal_cli(void);
cpu_flags_t hal_save_irq(void);
void  hal_restore_irq(cpu_flags_t flags);

uint64_t hal_get_mem_size(uint64_t mb_info);
err_t hal_timer_init(uint32_t hz);
uint64_t hal_timer_get_ticks(void);
uint64_t hal_timer_get_ns(void);
uint32_t hal_timer_get_hz(void);

char hal_uart_getchar(void);
int  hal_uart_data_available(void);
err_t hal_uart_rx_init(void);

void hal_poweroff(void);
void hal_reboot(void);
void hal_set_kernel_stack(uint64_t rsp0);
void hal_enable_irqs(void);
uint64_t hal_get_kernel_stack(void);
int  hal_smap_enabled(void);

extern uint64_t isr_vectors[256];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void);
extern void isr35(void); extern void isr36(void); extern void isr37(void);
extern void isr38(void); extern void isr39(void); extern void isr40(void);
extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void);
extern void isr47(void);
extern void isr128(void);

/* Standardised memory-map entry — filled by hal_get_mem_size, consumed by PMM */
#define MAX_MMAP_ENTRIES 32
typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t type;
} hal_mmap_entry_t;
int hal_get_mmap_entries(hal_mmap_entry_t* out, int max);

/* Multiboot info structures — defined once here to prevent drifing */
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

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v) {
    asm volatile("outw %0, %1" : : "a"(v), "dN"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    asm volatile("outb %0, %1" : : "a"(v), "dN"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v) {
    asm volatile("outl %0, %1" : : "a"(v), "dN"(port));
}

#endif
