#include "apic.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "hpet.h"
#include "smp.h"
#include "barrier.h"

int apic_present = 0;
int apic_x2apic = 0;
uint32_t apic_id = 0;

static volatile uint32_t* apic_mmio = NULL;

/* Read MSR */
uint64_t apic_read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Write MSR */
void apic_write_msr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
}

/* Check for APIC via CPUID */
static int cpuid_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1 << 9)) != 0;
}

/* Check for x2APIC via CPUID */
__attribute__((unused))
static int cpuid_has_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ecx & (1 << 21)) != 0;
}

/* x2APIC read (MSR-based).
 * x2APIC MSR address = 0x800 + (MMIO offset >> 4).
 * See Intel SDM Vol 3, Table 10-5. */
static uint32_t x2apic_read(unsigned reg) {
    uint32_t msr = 0x800 + (reg >> 4);
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"((uint32_t)msr));
    return lo;
}

/* x2APIC write (MSR-based) */
static void x2apic_write(unsigned reg, uint32_t val) {
    uint32_t msr = 0x800 + (reg >> 4);
    asm volatile("wrmsr" : : "a"(val), "d"((uint32_t)0), "c"((uint32_t)msr));
    asm volatile("mfence" ::: "memory");
}

uint32_t apic_read(unsigned reg) {
    if (apic_x2apic)
        return x2apic_read(reg);
    if (!apic_mmio) return 0;
    return *(volatile uint32_t*)((uintptr_t)apic_mmio + reg);
}

void apic_write(unsigned reg, uint32_t val) {
    if (apic_x2apic) {
        x2apic_write(reg, val);
        return;
    }
    if (!apic_mmio) return;
    *(volatile uint32_t*)((uintptr_t)apic_mmio + reg) = val;
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
    uint64_t apic_base_msr = apic_read_msr(IA32_APIC_BASE_MSR);
    uint64_t apic_base_phys = apic_base_msr & APIC_BASE_PHYS_MASK;

    kprintf("[APIC] Base MSR=%llx, phys=%llx\n", apic_base_msr, apic_base_phys);

    /* Check for x2APIC support. We keep x2APIC detection and MSR infrastructure
     * for future use, but for now use xAPIC MMIO everywhere — KVM properly
     * handles APIC MMIO via VMCS controls and TCG is excluded below. */

    /* Fall back to xAPIC MMIO */
    if (hal_is_qemu_tcg()) {
        kprintf("[APIC] QEMU TCG detected — skipping MMIO mapping (softmmu cache workaround), using legacy PIC\n");
        return ERR_NOENT;
    }

#define APIC_VADDR 0xFFFFFFFFFFFFE000ULL

    err_t err = vmm_map_page(vmm_get_kernel_pml4(), APIC_VADDR, apic_base_phys,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    if (err != ERR_OK) {
        kprintf("[APIC] Failed to map MMIO at %llx\n", apic_base_phys);
        return err;
    }

    apic_mmio = (volatile uint32_t*)APIC_VADDR;

    /* Enable APIC globally (bit 11) */
    apic_base_msr |= APIC_BASE_ENABLE;
    apic_write_msr(IA32_APIC_BASE_MSR, apic_base_msr);

    /* Read APIC version and ID */
    uint32_t version = apic_read(APIC_REG_VERSION);
    apic_id = apic_read(APIC_REG_ID) >> 24;

    kprintf("[APIC] Local APIC ID=%u, version=%u, max_lvt=%u\n",
            apic_id, version & 0xFF, (version >> 16) & 0xFF);

    apic_x2apic = 0;
    apic_present = 1;
    return ERR_OK;
}

void apic_enable(void) {
    if (!apic_present) return;
    /* Set SVR with spurious vector and enable */
    uint32_t svr = apic_read(APIC_REG_SVR);
    svr = (svr & ~0xFF) | APIC_SPURIOUS_VEC;
    svr |= APIC_SVR_ENABLE;
    svr &= ~APIC_SVR_FOCUS;
    apic_write(APIC_REG_SVR, svr);
}

void apic_disable(void) {
    if (!apic_present) return;
    uint64_t apic_base_msr = apic_read_msr(IA32_APIC_BASE_MSR);
    apic_base_msr &= ~(APIC_BASE_ENABLE | APIC_BASE_X2APIC);
    apic_write_msr(IA32_APIC_BASE_MSR, apic_base_msr);
    apic_present = 0;
    apic_x2apic = 0;
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
    apic_write(APIC_REG_TIMER_DIV, 0x03);

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
    uint32_t lint0 = apic_read(APIC_REG_LVT_LINT0);
    lint0 &= ~APIC_LVT_MASKED;
    lint0 = (lint0 & ~0x700) | APIC_LVT_DELIV_EXTINT;
    lint0 &= ~APIC_LVT_IRQ_POL;
    lint0 &= ~APIC_LVT_TRIGGER;
    apic_write(APIC_REG_LVT_LINT0, lint0);

    uint32_t lint1 = apic_read(APIC_REG_LVT_LINT1);
    lint1 &= ~APIC_LVT_MASKED;
    lint1 = (lint1 & ~0x700) | APIC_LVT_DELIV_NMI;
    apic_write(APIC_REG_LVT_LINT1, lint1);

    uint8_t pic1_mask = inb(0x21);
    /* Keep PIT unmasked — PIC ExtINT can serve as SMP fallback timer. */
    outb(0x21, pic1_mask);

    kprintf("[APIC] Legacy PIC reconfigured: PIT unmasked, LINT0=ExtINT, LINT1=NMI\n");
}

/* ----- IPI Support ----- */

void apic_send_ipi(uint32_t apic_id_dest, uint8_t vector, uint32_t delivery_mode) {
    if (apic_x2apic) {
        uint64_t icr = (uint64_t)apic_id_dest << 32;
        icr |= vector | delivery_mode | APIC_ICR_DEST_SHORT;
        apic_write_msr(0x830, icr);
    } else {
        apic_write(APIC_REG_ICR1, apic_id_dest << 24);
        asm volatile("mfence" ::: "memory");
        apic_write(APIC_REG_ICR0, vector | delivery_mode | APIC_ICR_DEST_SHORT);
        /* Delivery status wait skipped for cross-CPU IPIs — on real hardware
         * the delivery completes in microseconds and we don't write ICR0 again
         * until the next IPI. On KVM the delivery status never clears for
         * non-self targets. */
    }
}

void apic_send_ipi_self(uint8_t vector) {
    if (apic_x2apic) {
        apic_write_msr(0x830, (uint64_t)vector | APIC_ICR_DEST_SELF);
    } else {
        apic_write(APIC_REG_ICR0, vector | APIC_ICR_DEST_SELF);
        for (int i = 0; i < 100000; i++) {
            if (!(apic_read(APIC_REG_ICR0) & APIC_ICR_DELIV_STATUS))
                break;
            cpu_relax();
        }
    }
}

void apic_send_ipi_allbutself(uint8_t vector) {
    if (apic_x2apic) {
        uint64_t icr = (uint64_t)vector | APIC_ICR_DELIV_FIXED | APIC_ICR_DEST_ALL_OTHER;
        apic_write_msr(0x830, icr);
    } else {
        apic_write(APIC_REG_ICR0, (uint32_t)vector | APIC_ICR_DELIV_FIXED | APIC_ICR_DEST_ALL_OTHER);
        /* Delivery status wait skipped (same rationale as apic_send_ipi) */
    }
}

void apic_send_nmi_allbutself(void) {
    if (apic_x2apic) {
        uint64_t icr = (uint64_t)APIC_ICR_DELIV_NMI | APIC_ICR_DEST_ALL_OTHER;
        apic_write_msr(0x830, icr);
    } else {
        apic_write(APIC_REG_ICR0, (uint32_t)APIC_ICR_DELIV_NMI | APIC_ICR_DEST_ALL_OTHER);
    }
}

void apic_send_init_ipi(uint32_t apic_id_dest) {
    if (apic_x2apic) {
        uint64_t icr = (uint64_t)apic_id_dest << 32;
        icr |= APIC_ICR_DELIV_INIT | APIC_ICR_LEVEL_ASSERT | APIC_ICR_TRIGGER_LEVEL;
        apic_write_msr(0x830, icr);
    } else {
        apic_write(APIC_REG_ICR1, apic_id_dest << 24);
        asm volatile("mfence" ::: "memory");
        apic_write(APIC_REG_ICR0, APIC_ICR_DELIV_INIT | APIC_ICR_LEVEL_ASSERT
                   | APIC_ICR_TRIGGER_LEVEL);
        for (int i = 0; i < 100000; i++) {
            if (!(apic_read(APIC_REG_ICR0) & APIC_ICR_DELIV_STATUS))
                break;
            cpu_relax();
        }
    }
}

void apic_send_sipi_ipi(uint32_t apic_id_dest, uint8_t vector) {
    if (apic_x2apic) {
        uint64_t icr = (uint64_t)apic_id_dest << 32;
        icr |= vector | APIC_ICR_DELIV_STARTUP;
        apic_write_msr(0x830, icr);
    } else {
        apic_write(APIC_REG_ICR1, apic_id_dest << 24);
        asm volatile("mfence" ::: "memory");
        apic_write(APIC_REG_ICR0, (uint32_t)vector | APIC_ICR_DELIV_STARTUP);
        for (int i = 0; i < 100000; i++) {
            if (!(apic_read(APIC_REG_ICR0) & APIC_ICR_DELIV_STATUS))
                break;
            cpu_relax();
        }
    }
}

/* ----- I/O APIC ----- */

#define IOAPIC_IOREGSEL 0x00
#define IOAPIC_IOWIN    0x04
#define IOAPIC_VER      0x01
#define IOAPIC_REDIR_TBL 0x10
#define IOAPIC_VADDR    0xFFFFFFFFFFFFD000ULL

static uint32_t ioapic_read(uint64_t base_vaddr, uint8_t reg) {
    volatile uint32_t* ioregsel = (volatile uint32_t*)(uintptr_t)base_vaddr;
    volatile uint32_t* iowin    = (volatile uint32_t*)(uintptr_t)(base_vaddr + 4);
    *ioregsel = reg;
    asm volatile("mfence" ::: "memory");
    return *iowin;
}

static void ioapic_write(uint64_t base_vaddr, uint8_t reg, uint32_t val) {
    volatile uint32_t* ioregsel = (volatile uint32_t*)(uintptr_t)base_vaddr;
    volatile uint32_t* iowin    = (volatile uint32_t*)(uintptr_t)(base_vaddr + 4);
    *ioregsel = reg;
    asm volatile("mfence" ::: "memory");
    *iowin = val;
    asm volatile("mfence" ::: "memory");
}

void apic_ioapic_init(void) {
    if (io_apic_count == 0) {
        kprintf("[APIC] No I/O APICs found in MADT, using legacy PIC\n");
        return;
    }

    /* Map the first I/O APIC MMIO */
    uint64_t ioapic_phys = (uint64_t)io_apics[0].address;
    uint32_t gsi_base = io_apics[0].gsi_base;

    /* On KVM with in-kernel IOAPIC, MMIO reads at the ACPI-reported address
     * return 0 (the in-kernel emulation doesn't expose registers via MMIO).
     * Detect this by reading the version register — if it's 0, fall back
     * to legacy PIC.  On TCG and bare metal, I/O APIC MMIO works correctly. */

    err_t err = vmm_map_page(vmm_get_kernel_pml4(), IOAPIC_VADDR, ioapic_phys,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    if (err != ERR_OK) {
        kprintf("[APIC] Failed to map I/O APIC MMIO at 0x%llx\n", ioapic_phys);
        return;
    }

    /* Check if I/O APIC is accessible: KVM in-kernel IOAPIC returns 0 for
     * all MMIO reads.  Detect by checking the version register. */
    uint32_t ver = ioapic_read(IOAPIC_VADDR, IOAPIC_VER);
    if ((ver & 0xFF) == 0) {
        kprintf("[APIC] I/O APIC at 0x%llx: version=0 (KVM in-kernel or inaccessible)"
                " — using legacy PIC\n", ioapic_phys);
        vmm_unmap_page(vmm_get_kernel_pml4(), IOAPIC_VADDR);
        return;
    }
    uint32_t max_redir = (ver >> 16) & 0xFF;
    kprintf("[APIC] I/O APIC 0: ID=%u, version=%u, max_redir=%u, GSI base=%u\n",
            io_apics[0].id, ver & 0xFF, max_redir, gsi_base);

    for (uint32_t irq = 0; irq <= max_redir && irq < 16; irq++) {
        uint32_t dest = 0;
        int polarity = 0, trigger = 0;
        for (int s = 0; s < iso_count; s++) {
            if (isos[s].source == irq) {
                polarity = (isos[s].flags & 2) ? 1 : 0;
                trigger  = (isos[s].flags & 8) ? 1 : 0;
                kprintf("[APIC]   ISO IRQ%u → GSI%u, flags=%u (pol=%s trig=%s)\n",
                        irq, isos[s].gsi, isos[s].flags,
                        polarity ? "low" : "high", trigger ? "level" : "edge");
                break;
            }
        }
        uint32_t low = 32 + irq;
        if (polarity) low |= (1 << 13);
        if (trigger)  low |= (1 << 15);
        low |= (1 << 16);
        ioapic_write(IOAPIC_VADDR, IOAPIC_REDIR_TBL + 2 * irq, low);
        ioapic_write(IOAPIC_VADDR, IOAPIC_REDIR_TBL + 2 * irq + 1, dest << 24);
    }
    kprintf("[APIC] I/O APIC programmed for IRQs 0-%u\n",
            (max_redir < 15) ? max_redir : 15);
}
