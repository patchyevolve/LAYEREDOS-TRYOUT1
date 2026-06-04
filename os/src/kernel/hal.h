#ifndef HAL_H
#define HAL_H

#include "types.h"

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} int_frame_t;

typedef void (*irq_handler_t)(int_frame_t* frame, void* data);

err_t hal_init(uint64_t mb_info_phys);
err_t hal_idt_set_gate(int vec, void* handler, uint8_t dpl);
err_t hal_irq_register(uint8_t irq, irq_handler_t handler, void* data);
err_t hal_irq_unregister(uint8_t irq);
void  hal_irq_eoi(uint8_t irq);
void  hal_sti(void);
void  hal_cli(void);
cpu_flags_t hal_save_irq(void);
void  hal_restore_irq(cpu_flags_t flags);

uint64_t hal_get_mem_size(uint64_t mb_info);
err_t hal_timer_init(uint32_t hz);
uint64_t hal_timer_get_ticks(void);
uint64_t hal_timer_get_ns(void);

void hal_uart_putchar(char c);
char hal_uart_getchar(void);
int  hal_uart_data_available(void);

void hal_poweroff(void);
void hal_reboot(void);

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

#endif
