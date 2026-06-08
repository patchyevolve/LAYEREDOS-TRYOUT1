#include "apic.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "hpet.h"

int apic_present = 0;
uint32_t apic_id = 0;

static volatile uint32_t* apic_mmio = NULL;

/* Map a physical address into kernel virtual space at a given VA */
static err_t mmio_map_page(uint64_t phys, uint64_t virt) {
    return vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
}

/* Read MSR */
static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Write MSR */
static void write_msr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
}

/* Check for APIC via CPUID */
static int cpuid_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1 << 9)) != 0;
}

uint32_t apic_read(unsigned reg) {
    if (!apic_mmio) return 0;
    return *(volatile uint32_t*)((uintptr_t)apic_mmio + reg);
}

void apic_write(unsigned reg, uint32_t val) {
    if (!apic_mmio) return;
    *(volatile uint32_t*)((uintptr_t)apic_mmio + reg) = val;
    /* Ensure write is ordered */
    asm volatile("mfence" ::: "memory");
}

void apic_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}

err_t apic_init(void) {
    if (!cpuid_has_apic()) {
        kprintf("[APIC] No local APIC detected via CPUID\n");
        return ERR_NOENT;
    }

    /* Read APIC base MSR to get the physical base address */
    uint64_t apic_base_msr = read_msr(IA32_APIC_BASE_MSR);
    uint64_t apic_base_phys = apic_base_msr & APIC_BASE_PHYS_MASK;

    kprintf("[APIC] Base MSR=%llx, phys=%llx\n", apic_base_msr, apic_base_phys);

    /* Map APIC MMIO region (one page is sufficient)
     * Use virtual address at 0xFFFFFFFFFFFFE000 (one page below HPET at 0xFFFFFFFFFFFFF000) */
    #define APIC_VADDR 0xFFFFFFFFFFFFE000ULL

    err_t err = mmio_map_page(apic_base_phys, APIC_VADDR);
    if (err != ERR_OK) {
        kprintf("[APIC] Failed to map MMIO at %llx\n", apic_base_phys);
        return err;
    }

    apic_mmio = (volatile uint32_t*)APIC_VADDR;

    /* Enable APIC globally (bit 11) */
    apic_base_msr |= APIC_BASE_ENABLE;
    write_msr(IA32_APIC_BASE_MSR, apic_base_msr);

    /* Read APIC version and ID */
    uint32_t version = apic_read(APIC_REG_VERSION);
    apic_id = apic_read(APIC_REG_ID) >> 24;

    kprintf("[APIC] Local APIC ID=%u, version=%u, max_lvt=%u\n",
            apic_id, version & 0xFF, (version >> 16) & 0xFF);

    apic_present = 1;
    return ERR_OK;
}

void apic_enable(void) {
    if (!apic_present) return;
    /* Set SVR with spurious vector and enable */
    uint32_t svr = apic_read(APIC_REG_SVR);
    svr = (svr & ~0xFF) | APIC_SPURIOUS_VEC;
    svr |= APIC_SVR_ENABLE;
    svr &= ~APIC_SVR_FOCUS; /* disable focus processor */
    apic_write(APIC_REG_SVR, svr);
}

void apic_disable(void) {
    if (!apic_present) return;
    /* Clear enable bit in APIC base MSR */
    uint64_t apic_base_msr = read_msr(IA32_APIC_BASE_MSR);
    apic_base_msr &= ~APIC_BASE_ENABLE;
    write_msr(IA32_APIC_BASE_MSR, apic_base_msr);
}

static uint32_t apic_calibrate_init_count(uint32_t hz) {
    uint64_t probe_count = 0xFFFFFFFFULL;
    apic_write(APIC_REG_TIMER_INIT, (uint32_t)probe_count);
    uint64_t start_ns = hpet_ns();
    while (hpet_ns() - start_ns < 1000000);
    uint32_t remaining = apic_read(APIC_REG_TIMER_CUR);
    uint64_t ticks_per_ms = (probe_count - remaining) / 1;
    uint32_t init_count = (uint32_t)(ticks_per_ms * 1000 / hz);
    if (init_count == 0) init_count = 1;
    kprintf("[APIC] Calibrated: %llu ticks/ms, init_count=%u for %u Hz\n",
            ticks_per_ms, init_count, hz);
    return init_count;
}

void apic_timer_init(uint32_t hz) {
    if (!apic_present || hz == 0) return;

    /* APIC timer runs at bus frequency (typically ~100-400 MHz).
     * Use divider 16. */
    apic_write(APIC_REG_TIMER_DIV, 0x03); /* divider = 16 (0x3 means divide by 16) */

    /* Set timer LVT: periodic, unmasked, vector 32 (IRQ0) */
    uint32_t lvt = 32 | APIC_LVT_TIMER_PERIODIC;
    apic_write(APIC_REG_LVT_TIMER, lvt);

    uint32_t init_count;
    if (hpet_present) {
        init_count = apic_calibrate_init_count(hz);
    } else {
        init_count = 12500;
    }
    apic_write(APIC_REG_TIMER_INIT, init_count);

    kprintf("[APIC] Timer initialized for %u Hz (init_count=%u)\n", hz, init_count);
}

void apic_disable_pic(void) {
    /* Configure LINT0 for ExtINT delivery (pass-through from PIC) */
    uint32_t lint0 = apic_read(APIC_REG_LVT_LINT0);
    lint0 &= ~APIC_LVT_MASKED;
    lint0 = (lint0 & ~0x700) | APIC_LVT_DELIV_EXTINT;
    lint0 &= ~APIC_LVT_IRQ_POL; /* active high */
    lint0 &= ~APIC_LVT_TRIGGER; /* edge triggered */
    apic_write(APIC_REG_LVT_LINT0, lint0);

    /* Configure LINT1 for NMI */
    uint32_t lint1 = apic_read(APIC_REG_LVT_LINT1);
    lint1 &= ~APIC_LVT_MASKED;
    lint1 = (lint1 & ~0x700) | APIC_LVT_DELIV_NMI;
    apic_write(APIC_REG_LVT_LINT1, lint1);

    /* Mask IRQ0 (PIT timer) since we use APIC timer instead.
     * Keep other IRQs unmasked (keyboard, ATA, UART still go through PIC). */
    uint8_t pic1_mask = inb(0x21);
    pic1_mask |= (1 << 0); /* mask IRQ0 (timer) */
    outb(0x21, pic1_mask);

    kprintf("[APIC] Legacy PIC reconfigured: PIT masked, LINT0=ExtINT, LINT1=NMI\n");
}