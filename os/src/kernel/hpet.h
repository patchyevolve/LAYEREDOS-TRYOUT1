#ifndef HPET_H
#define HPET_H

#include "types.h"

#define HPET_MMIO_BASE  0xFED00000ULL
#define HPET_MMIO_SIZE  0x1000

/* HPET register offsets (from MMIO base) */
#define HPET_GEN_CAP    0x000  /* General capabilities */
#define HPET_GEN_CONF   0x010  /* General configuration */
#define HPET_INT_STAT   0x020  /* General interrupt status */
#define HPET_MAIN_CNT   0x0F0  /* Main counter */
#define HPET_T0_CONF    0x100  /* Timer 0 config */
#define HPET_T0_COMP    0x108  /* Timer 0 comparator */
#define HPET_T0_FSB     0x110  /* Timer 0 FSB route */
#define HPET_T1_CONF    0x120
#define HPET_T1_COMP    0x128
#define HPET_T1_FSB     0x130
#define HPET_T2_CONF    0x140
#define HPET_T2_COMP    0x148
#define HPET_T2_FSB     0x150

/* Capability register fields */
#define HPET_CAP_REV        0xFF000000ULL
#define HPET_CAP_NUM_TIMERS 0x00001F00ULL
#define HPET_CAP_WIDTH      0x00000004ULL /* 1 = 64-bit, 0 = 32-bit */
#define HPET_CAP_LEG_ROUTE  0x00000008ULL
#define HPET_CAP_PERIOD     0xFFFFFFFF00000000ULL
#define HPET_CAP_GET_PERIOD(cap) ((cap) >> 32)
#define HPET_CAP_GET_FS_CLK(cap) (((cap) >> 32) & 0xFFFFFFFFULL)
#define HPET_FS_CLK(fs)      ((fs) ? 100000000000000000ULL / (fs) : 0) /* femto to Hz */

/* Configuration register fields */
#define HPET_CONF_ENABLE    0x001ULL
#define HPET_CONF_LEG_RT    0x002ULL

/* Timer config fields */
#define HPET_TN_TYPE        0x001ULL /* 0=one-shot, 1=periodic */
#define HPET_TN_INT_ENB     0x004ULL
#define HPET_TN_INT_TYPE    0x008ULL /* 0=level, 1=edge */
#define HPET_TN_FSB_EN      0x040ULL
#define HPET_TN_VAL_SET     0x100ULL /* value set (periodic accumulator) */
#define HPET_TN_SIZE_CAP    0x200ULL /* 0=32-bit, 1=64-bit */
#define HPET_TN_ROUTE       0x3E00ULL
#define HPET_TN_FSB_INT     0x8000ULL
#define HPET_TN_IRQ_ROUTE   0xFF00000000000000ULL

extern int hpet_present;
extern int hpet_is_64bit;
extern uint64_t hpet_hz;

err_t hpet_init(void);
void hpet_enable(void);
void hpet_disable(void);
uint64_t hpet_read_counter(void);
uint64_t hpet_ns(void);
void hpet_timer_init(void);

#endif