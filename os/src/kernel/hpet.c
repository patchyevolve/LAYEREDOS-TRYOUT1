#include "hpet.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"

int hpet_present = 0;
int hpet_is_64bit = 0;
uint64_t hpet_hz = 0;
static volatile uint64_t* hpet_regs = NULL;
static uint64_t hpet_period_fs = 0; /* period in femtoseconds */

/* Map a physical address into the kernel's virtual address space */
#define HPET_VADDR 0xFFFFFFFFFFFFF000ULL

static volatile uint64_t* hpet_map_mmio(uint64_t phys, size_t size) {
    if (hal_is_qemu_tcg()) {
        kprintf("[HPET] QEMU TCG detected — skipping MMIO mapping (softmmu cache workaround), using PIT\n");
        return NULL;
    }

    uint64_t va = HPET_VADDR;
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t pa = phys + i * PAGE_SIZE;
        uint64_t vaddr = va + i * PAGE_SIZE;
        err_t err = vmm_map_page(vmm_get_kernel_pml4(), vaddr, pa,
                                 PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (err != ERR_OK) {
            kprintf("[HPET] Failed to map MMIO at %llx\n", pa);
            return NULL;
        }
    }
    return (volatile uint64_t*)va;
}

static inline uint64_t hpet_read(unsigned off) {
    return hpet_regs[off / 8];
}

static inline void hpet_write(unsigned off, uint64_t val) {
    hpet_regs[off / 8] = val;
}

static inline uint32_t hpet_read32(unsigned off) {
    return *(volatile uint32_t*)((uintptr_t)hpet_regs + off);
}

err_t hpet_init(void) {
    /* Map the HPET MMIO region */
    hpet_regs = hpet_map_mmio(HPET_MMIO_BASE, HPET_MMIO_SIZE);
    if (!hpet_regs) {
        return ERR_FAULT;
    }

    /* Read capabilities */
    uint64_t cap = hpet_read(HPET_GEN_CAP);
    if (cap == 0 || cap == 0xFFFFFFFFFFFFFFFFULL) {
        kprintf("[HPET] No HPET found at %llx (cap=%llx)\n",
                (uint64_t)HPET_MMIO_BASE, cap);
        hpet_regs = NULL;
        return ERR_NOENT;
    }

    hpet_is_64bit = (cap & HPET_CAP_WIDTH) ? 1 : 0;

    /* Get period in femtoseconds */
    hpet_period_fs = HPET_CAP_GET_PERIOD(cap);
    if (hpet_period_fs == 0) {
        kprintf("[HPET] Invalid period (0)\n");
        return ERR_FAULT;
    }

    /* Calculate frequency: 1e15 fs/sec / period_fs = Hz */
    hpet_hz = 1000000000000000ULL / hpet_period_fs;

    /* Number of timers */
    int num_timers = ((cap & HPET_CAP_NUM_TIMERS) >> 8) + 1;
    int legacy_route = (cap & HPET_CAP_LEG_ROUTE) ? 1 : 0;

    kprintf("[HPET] Detected: %d timers, %d-bit, period=%llu fs (%llu Hz), leg_route=%d\n",
            num_timers, hpet_is_64bit ? 64 : 32, hpet_period_fs, hpet_hz, legacy_route);

    /* Reset counter */
    hpet_write(HPET_MAIN_CNT, 0);

    hpet_present = 1;
    return ERR_OK;
}

void hpet_enable(void) {
    if (!hpet_present) return;
    uint64_t conf = hpet_read(HPET_GEN_CONF);
    conf |= HPET_CONF_ENABLE;
    hpet_write(HPET_GEN_CONF, conf);
}

void hpet_disable(void) {
    if (!hpet_present) return;
    uint64_t conf = hpet_read(HPET_GEN_CONF);
    conf &= ~HPET_CONF_ENABLE;
    hpet_write(HPET_GEN_CONF, conf);
}

uint64_t hpet_read_counter(void) {
    if (!hpet_present) return 0;
    if (hpet_is_64bit) {
        uint32_t hi_1, lo, hi_2;
        do {
            hi_1 = hpet_read32(HPET_MAIN_CNT + 4);
            lo    = hpet_read32(HPET_MAIN_CNT);
            hi_2 = hpet_read32(HPET_MAIN_CNT + 4);
        } while (hi_1 != hi_2);
        return ((uint64_t)hi_1 << 32) | lo;
    }
    return hpet_read32(HPET_MAIN_CNT);
}

uint64_t hpet_ns(void) {
    if (!hpet_present || hpet_period_fs == 0) return 0;
    /* Convert counter to nanoseconds */
    uint64_t count = hpet_read_counter();
    /* count * period_fs / 1e6 = nanoseconds
     * Use 128-bit intermediate: (count * period_fs) / 1000000
     * But to avoid overflow, do in parts */
    __uint128_t ns = (__uint128_t)count * hpet_period_fs / 1000000ULL;
    return (uint64_t)ns;
}

void hpet_timer_init(void) {
    if (!hpet_present) return;

    kprintf("[HPET] Configuring timer 0 for periodic mode\n");

    /* Disable HPET while configuring */
    hpet_disable();

    /* Configure timer 0:
     * - Periodic mode (bit 0 = 1)
     * - Interrupt enable (bit 2, but we'll use polling for now)
     * - 32-bit mode (clear bit 8 to use 32-bit comparator) */
    uint64_t t0conf = HPET_TN_TYPE; /* periodic */

    /* Route to IRQ 2 (PIT's IRQ, legacy replacement) */
    if (hpet_read(HPET_GEN_CAP) & HPET_CAP_LEG_ROUTE) {
        t0conf |= HPET_TN_INT_ENB;
    }

    hpet_write(HPET_T0_CONF, t0conf);

    /* Set comparator for ~1ms interval (hz/1000 ticks) */
    uint64_t cmp = hpet_hz / 1000;
    if (!(hpet_read(HPET_T0_CONF) & HPET_TN_SIZE_CAP)) cmp &= 0xFFFFFFFFULL;
    hpet_write(HPET_T0_COMP, cmp);
    hpet_write(HPET_T0_CONF, t0conf | HPET_TN_VAL_SET);

    /* Reset and enable counter */
    hpet_write(HPET_MAIN_CNT, 0);
    hpet_enable();

    kprintf("[HPET] Timer initialized at %llu Hz, comparator=%llu\n", hpet_hz, cmp);
}