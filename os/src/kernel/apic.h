#ifndef APIC_H
#define APIC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IA32_APIC_BASE MSR */
#define IA32_APIC_BASE_MSR  0x1B
#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_BSP       (1ULL << 8)
#define APIC_BASE_X2APIC    (1ULL << 10)
#define APIC_BASE_PHYS_MASK 0xFFFFFFFFFFFFF000ULL

/* Default APIC MMIO base */
#define APIC_DEFAULT_BASE   0xFEE00000ULL

/* APIC register offsets (from MMIO base) */
#define APIC_REG_ID         0x020
#define APIC_REG_VERSION    0x030
#define APIC_REG_TPR        0x080
#define APIC_REG_APR        0x090
#define APIC_REG_PPR        0x0A0
#define APIC_REG_EOI        0x0B0
#define APIC_REG_RRD        0x0C0
#define APIC_REG_SVR        0x0F0
#define APIC_REG_ISR0       0x100
#define APIC_REG_TMR0       0x180
#define APIC_REG_IRR0       0x200
#define APIC_REG_ESR        0x280
#define APIC_REG_ICR0       0x300
#define APIC_REG_ICR1       0x310
#define APIC_REG_LVT_TIMER  0x320
#define APIC_REG_LVT_THERM  0x330
#define APIC_REG_LVT_PERF   0x340
#define APIC_REG_LVT_LINT0  0x350
#define APIC_REG_LVT_LINT1  0x360
#define APIC_REG_LVT_ERROR  0x370
#define APIC_REG_TIMER_INIT 0x380
#define APIC_REG_TIMER_CUR  0x390
#define APIC_REG_TIMER_DIV  0x3E0

/* x2APIC MSR base */
#define X2APIC_MSR_BASE     0x800
#define X2APIC_ICR_MSR      0x830

/* SVR bits */
#define APIC_SVR_ENABLE     (1ULL << 8)
#define APIC_SVR_FOCUS      (1ULL << 9)

/* LVT bits */
#define APIC_LVT_MASKED     (1ULL << 16)
#define APIC_LVT_TRIGGER    (1ULL << 15)
#define APIC_LVT_IRQ_POL    (1ULL << 13)
#define APIC_LVT_DELIV_STATUS (1ULL << 12)
#define APIC_LVT_DELIV_SHIFT 8
#define APIC_LVT_DELIV_FIXED  0
#define APIC_LVT_DELIV_NMI    (4 << 8)
#define APIC_LVT_DELIV_EXTINT (7 << 8)
#define APIC_LVT_TIMER_PERIODIC (1ULL << 17)
#define APIC_LVT_TIMER_ONESHOT  0
#define APIC_LVT_TIMER_TSCDEADLINE (2ULL << 17)

/* ICR delivery modes */
#define APIC_ICR_DELIV_FIXED  0
#define APIC_ICR_DELIV_LOWPRI (1ULL << 8)
#define APIC_ICR_DELIV_SMI    (2ULL << 8)
#define APIC_ICR_DELIV_NMI    (4ULL << 8)
#define APIC_ICR_DELIV_INIT   (5ULL << 8)
#define APIC_ICR_DELIV_STARTUP (6ULL << 8)
#define APIC_ICR_DEST_MODE    (1ULL << 11)
#define APIC_ICR_DELIV_STATUS (1ULL << 12)
#define APIC_ICR_LEVEL_ASSERT (1ULL << 14)
#define APIC_ICR_TRIGGER_LEVEL (1ULL << 15)
#define APIC_ICR_DEST_SHORT    (0ULL << 18)
#define APIC_ICR_DEST_SELF     (1ULL << 18)
#define APIC_ICR_DEST_ALL      (2ULL << 18)
#define APIC_ICR_DEST_ALL_OTHER (3ULL << 18)

/* Spurious vector */
#define APIC_SPURIOUS_VEC 0xFF

extern int apic_present;
extern int apic_x2apic;
extern uint32_t apic_id;

/* Core APIC */
void     apic_reset(void);    /* clear stale state after warm reset */
err_t    apic_init(void);
void     apic_enable(void);
void     apic_disable(void);
void     apic_eoi(void);
uint32_t apic_read(unsigned reg);
void     apic_write(unsigned reg, uint32_t val);
uint64_t apic_read_msr(uint32_t msr);
void     apic_write_msr(uint32_t msr, uint64_t val);

/* APIC timer */
void     apic_timer_init(uint32_t hz);
void     apic_disable_pic(void);

/* IPI delivery */
void apic_send_ipi(uint32_t apic_id_dest, uint8_t vector, uint32_t delivery_mode);
void apic_send_ipi_self(uint8_t vector);
void apic_send_ipi_allbutself(uint8_t vector);
void apic_send_init_ipi(uint32_t apic_id_dest);
void apic_send_sipi_ipi(uint32_t apic_id_dest, uint8_t vector);

/* I/O APIC */
void apic_ioapic_init(void);

/* NMI IPI — sends NMI to all CPUs except self */
void apic_send_nmi_allbutself(void);

#ifdef __cplusplus
}
#endif

#endif /* APIC_H */
