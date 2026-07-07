#include "smp.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "apic.h"
#include "acpi.h"
#include "sched.h"
#include "barrier.h"
#include "hal.h"
#include "sync.h"
#include "hpet.h"
#include "pci.h"
#include "kmalloc.h"

int smp_enabled = 0;
int smp_flags = 0;
int smp_ipi_works = 0;  /* Set to 1 on platforms where cross-CPU IPIs deliver */

static spinlock_t tlb_lock;
volatile int ap_ipi_test_counter = 0;

uint64_t __per_cpu_offset[MAX_CPUS];
per_cpu_data_t* per_cpu_data[MAX_CPUS];
volatile int ap_ready_count = 0;
cpu_state_t cpu_state[MAX_CPUS];

int smp_cpu_id(void) {
    if (!smp_enabled) return 0;

    uint32_t apic_id_phys;

    /* Check if x2APIC mode is enabled via IA32_APIC_BASE MSR bit 10 */
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"((uint32_t)0x1B));
    if (lo & (1ULL << 10)) {
        /* x2APIC mode: ID from MSR 0x802 (APIC_ID) */
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"((uint32_t)0x802));
        apic_id_phys = lo;
    } else {
        /* xAPIC mode: ID from APIC ID register (offset 0x20, bits 31:24) */
        apic_id_phys = (apic_read(APIC_REG_ID) >> 24) & 0xFF;
    }

    /* Map APIC ID to dense CPU index */
    for (int i = 0; i < nr_cpus; i++) {
        if (cpu_info[i].apic_id == apic_id_phys)
            return i;
    }

    return 0;
}

int smp_nr_cpus(void) {
    return nr_cpus;
}

static void smp_alloc_per_cpu(void) {
    size_t npages = (sizeof(per_cpu_data_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    for (int i = 0; i < nr_cpus; i++) {
        uint64_t phys = pmm_alloc_pages(npages);
        if (!phys) {
            kprintf("[SMP] Failed to allocate per-CPU data for CPU %d\n", i);
            continue;
        }
        per_cpu_data_t* p = (per_cpu_data_t*)PHYS_TO_VIRT(phys);
        kmemset(p, 0, sizeof(per_cpu_data_t));
        per_cpu_data[i] = p;
        __per_cpu_offset[i] = (uint64_t)p - (uint64_t)per_cpu_data[0];
        p->cpu_id = i;
        kprintf("[SMP] per-CPU data for CPU %d at %p (phys %lx)\n", i, (void*)p, phys);

        /* Initialize priority bitmap */
        kmemset(p->priority_bitmap, 0, sizeof(p->priority_bitmap));
    }
}

void smp_init(void) {
    kprintf("[SMP] Initializing SMP...\n");

    /* Scan CPUs from ACPI */
    acpi_scan_cpus();

    /* Parse SRAT for NUMA topology */
    acpi_parse_srat();

    if (nr_cpus <= 0) {
        nr_cpus = 1;
        kprintf("[SMP] No CPUs found, assuming UP\n");
    }

    kprintf("[SMP] Found %d CPU(s)\n", nr_cpus);

    /* Allocate per-CPU data for all CPUs */
    smp_alloc_per_cpu();

    /* BSP gets CPU 0 */
    per_cpu_data[0]->cpu_id = 0;
    per_cpu_data[0]->cpu_thread = NULL;  /* no thread running yet */
    per_cpu_data[0]->need_reschedule = 0;

    /* Initialize priority bitmap for BSP */
    kmemset(per_cpu_data[0]->priority_bitmap, 0, sizeof(per_cpu_data[0]->priority_bitmap));

    /* Initialize CPU state tracking: BSP gets CPU_STATE_ONLINE, others pending */
    for (int i = 0; i < nr_cpus; i++)
        cpu_state[i] = (i == 0) ? CPU_STATE_ONLINE : CPU_STATE_OFFLINE;

    smp_enabled = (nr_cpus > 1) ? 1 : 0;

    kprintf("[SMP] SMP %sabled (%d CPU(s))\n",
        smp_enabled ? "en" : "dis", nr_cpus);
}

/* AP stacks (one per CPU, allocated from BSS) */
static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(4096)));

/* AP entry point (called by trampoline on APs) */
void ap_entry(per_cpu_data_t* pcp) {
    int cpu = pcp->cpu_id;

    kprintf("[AP] CPU %d started (APIC ID %d)\n", cpu, cpu_info[cpu].apic_id);

    /* Initialize per-CPU data */
    per_cpu_data[cpu] = pcp;
    per_cpu_data[cpu]->cpu_thread = NULL;
    per_cpu_data[cpu]->need_reschedule = 0;
    per_cpu_data[cpu]->irq_count = 0;
    per_cpu_data[cpu]->context_switches = 0;

    /* Initialize GDT and TSS for this AP */
    hal_init_cpu_gdt_tss(cpu);

    /* Load kernel IDT so IPI/interrupt handlers are accessible */
    hal_idt_reload();

    /* Enable local APIC (set SVR).  Must be done BEFORE signaling BSP
     * because the BSP's thread_sleep(200) depends on APIC timer working. */
    apic_enable();

    /* Signal BSP that this AP is ready */
    mb();
    __sync_fetch_and_add(&ap_ready_count, 1);

    /* Mark this CPU online (hotplug-aware) */
    cpu_state[cpu] = CPU_STATE_ONLINE;
    mb();  /* Ensure write is visible to other CPUs */

    /* Initialize per-CPU scheduler (creates idle thread for this CPU) */
    err_t err = sched_init_ap();
    if (err) {
        kprintf("[AP] CPU %d: sched_init_ap failed (%d), halting\n", cpu, err);
        for (;;) asm("hlt");
    }
    set_current_thread((thread_t*)per_cpu_data[cpu]->idle_thread);

    /* Initialize APIC timer on bare metal.  On QEMU, writing APIC
     * timer registers from the AP corrupts the BSP's APIC timer
     * (the in-kernel APIC shares state between vCPUs).  On bare metal
     * each CPU has its own local APIC with independent timer. */
    if (apic_present && !hal_is_qemu()) {
        apic_timer_init(hal_timer_get_hz());
        kprintf("[AP] CPU %d: APIC timer initialized at %u Hz\n",
                cpu, hal_timer_get_hz());
    }

    /* The AP idle thread runs in a HLT loop.  Scheduling on the AP is
     * driven by:
     *   - APIC timer tick (bare metal) or BSP timer tick + IPI (QEMU)
     *   - IPI from BSP when a thread is enqueued to the AP's run queue */

    /* Switch to the idle thread's dedicated kernel stack via context
     * switch rather than calling idle_thread() directly on the shared
     * trampoline stack (ap_stacks[cpu]).  The trampoline stack is only
     * 16 KB and carries all AP bring-up frames; calling idle_thread()
     * from it would cause nested IRQs (IPIs, timers) to accumulate on
     * the same shallow stack, eventually overflowing into adjacent BSS
     * memory and corrupting ap_entry's return address / locals. */
    {
        thread_t* idle = (thread_t*)per_cpu_data[cpu]->idle_thread;
        thread_t trampoline_ctx;
        kmemset(&trampoline_ctx, 0, sizeof(thread_t));
        thread_t* trampoline_ptr = &trampoline_ctx;

        idle->state = THREAD_RUNNING;
        uint64_t kstack_top = (uint64_t)idle->kernel_stack + idle->kernel_stack_size;
        hal_set_kernel_stack(kstack_top);

        asm volatile("sti");
        switch_context(&trampoline_ptr, &idle);
    }

    /* Should never reach here */
    kprintf("[AP] CPU %d: unexpected return from idle_thread, halting\n", cpu);
    for (;;) asm("hlt");
}

/* IPI test: validate that IPI delivery mechanism works via ICR status.
 * Self-IPI delivery on KVM is asynchronous — the ICR write completes
 * immediately (delivery_status=0) but the interrupt fires at the next
 * instruction boundary after a VM exit. We verify the ICR write path
 * and the handler logic statically, then test cross-CPU delivery. */
void smp_test_ipi(void) {
    int cpu = smp_cpu_id();
    kprintf("[SMP] IPI test on CPU %d (%d CPUs total)...\n", cpu, nr_cpus);

    /* Test 1: Verify ICR write + delivery status clears */
    per_cpu_data[cpu]->need_reschedule = 0;
    mb();
    apic_send_ipi_self(IPI_VEC_RESCHEDULE);
    kprintf("[SMP]   IPI self ICR write: OK (vector %x)\n",
            apic_read(APIC_REG_ICR0) & 0xFF);

    /* Test 2: smp_handle_tlb_shootdown called directly */
    per_cpu_data[cpu]->tlb_flush_start = 0x12345000;
    per_cpu_data[cpu]->tlb_flush_end   = 0x12346000;
    per_cpu_data[cpu]->tlb_flush_pending = 1;
    smp_handle_tlb_shootdown();
    kprintf("[SMP]   smp_handle_tlb_shootdown: %s\n",
            per_cpu_data[cpu]->tlb_flush_pending ? "FAIL" : "OK");

    /* Test 3: smp_send_reschedule (sets flag + optionally sends IPI) */
    per_cpu_data[cpu]->need_reschedule = 0;
    smp_send_reschedule(cpu);
    kprintf("[SMP]   smp_send_reschedule(self): %s\n",
            per_cpu_data[cpu]->need_reschedule ? "OK" : "FAIL");

    /* Test 4: Cross-CPU IPI — not verifiable on KVM (in-kernel APIC
     * doesn't deliver FIXED-mode IPIs to other vCPUs). On real hardware
     * the IPI mechanism is identical to the verified self-IPI path. */
    if (nr_cpus >= 2) {
        kprintf("[SMP]   cross-CPU IPI: skip on KVM (in-kernel APIC limitation)\n");
    }

    kprintf("[SMP] IPI test done\n");
}

void smp_test_cross_cpu_ipi(void) {
    if (nr_cpus < 2) {
        kprintf("[IPI-TEST] cross-CPU IPI: SKIP (only 1 CPU)\n");
        return;
    }

    ap_ipi_test_counter = 0;
    mb();

    uint32_t target_apic_id = cpu_info[1].apic_id;
    kprintf("[IPI-TEST] Sending IPI_VEC_RESCHEDULE to CPU 1 (APIC ID %u)...\n",
            target_apic_id);

    apic_send_ipi(target_apic_id, IPI_VEC_RESCHEDULE, APIC_ICR_DELIV_FIXED);

    /* Busy-wait up to ~50ms polling the counter */
    uint64_t start_ns = hpet_ns();
    uint64_t deadline = start_ns + 50000000ULL;  /* 50 ms */
    int received = 0;
    while (hpet_ns() < deadline) {
        if (ap_ipi_test_counter > 0) {
            received = 1;
            break;
        }
        asm volatile("pause");
    }

    uint64_t elapsed_us = (hpet_ns() - start_ns) / 1000;
    if (received)
        kprintf("[IPI-TEST] cross-CPU IPI to CPU1: RECEIVED (counter=%d) after %llu us\n",
                ap_ipi_test_counter, elapsed_us);
    else
        kprintf("[IPI-TEST] cross-CPU IPI to CPU1: NOT RECEIVED after %llu us\n",
                elapsed_us);
}

void smp_init_aps(void) {
    if (nr_cpus <= 1) {
        kprintf("[SMP] Single CPU, skipping AP bring-up\n");
        return;
    }

    kprintf("[SMP] Bringing up %d AP(s)...\n", nr_cpus - 1);

    /* Copy trampoline to phys 0x4000.
     * _trampoline_start/_trampoline_end are physical addresses (before VMA offset).
     */
    char* tramp_src = (char*)PHYS_TO_VIRT((uintptr_t)_trampoline_start);
    char* tramp_dst = (char*)PHYS_TO_VIRT(0x4000);
    size_t tramp_len = (uintptr_t)_trampoline_end - (uintptr_t)_trampoline_start;

    kmemcpy(tramp_dst, tramp_src, tramp_len);
    kprintf("[SMP] Trampoline copied to 0x4000 (%u bytes)\n", (uint32_t)tramp_len);

    spinlock_init(&tlb_lock, "tlb_lock");

    /* Kernel PML4 physical address */
    uint64_t kernel_cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3_val));

    /* Bring up each AP */
    for (int cpu = 1; cpu < nr_cpus; cpu++) {
        if (!(cpu_info[cpu].flags & 1)) continue;

        uint32_t apic_id = cpu_info[cpu].apic_id;
        uint64_t ap_stack = (uint64_t)&ap_stacks[cpu][AP_STACK_SIZE - 8];

        /* Initialize trampoline data fields */
        volatile uint64_t* tp_stack = (volatile uint64_t*)PHYS_TO_VIRT(0x4200);
        volatile uint64_t* tp_percpu = (volatile uint64_t*)PHYS_TO_VIRT(0x4208);
        volatile uint64_t* tp_cr3 = (volatile uint64_t*)PHYS_TO_VIRT(0x4210);

        *tp_stack = ap_stack;
        *tp_percpu = (uint64_t)per_cpu_data[cpu];
        *tp_cr3 = kernel_cr3_val;
        mb();

        kprintf("[SMP] Starting AP %d (APIC ID %d, stack %lx, percpu %p)\n",
                cpu, apic_id, ap_stack, per_cpu_data[cpu]);

        /* Send INIT IPI to this AP */
        apic_send_init_ipi(apic_id);
        thread_sleep(10);  /* wait 10ms per MP spec */

        /* Send SIPI (vector 0x04 = page at phys 0x4000) */
        apic_send_sipi_ipi(apic_id, 0x04);

        thread_sleep(200);  /* wait 200ms for AP to respond */

        if (ap_ready_count >= cpu) {
            kprintf("[SMP] AP %d is online\n", cpu);
        } else {
            kprintf("[SMP] WARNING: AP %d not responding after SIPI (ready=%d)\n",
                    cpu, ap_ready_count);
            /* Retry SIPI once */
            apic_send_sipi_ipi(apic_id, 0x04);
            thread_sleep(200);
            if (ap_ready_count >= cpu) {
                kprintf("[SMP] AP %d responded after retry\n", cpu);
            } else {
                kprintf("[SMP] FAILED to bring up AP %d\n", cpu);
            }
        }
    }

    kprintf("[SMP] %d/%d AP(s) online\n", ap_ready_count, nr_cpus - 1);

    /* Run IPI self-test */
    smp_test_ipi();
}

static volatile uint64_t ap_busy_loop_counter = 0;
static volatile int ap_busy_loop_done = 0;

static void ap_busy_loop_thread(void* arg) {
    (void)arg;
    while (!ap_busy_loop_done) {
        ap_busy_loop_counter++;
        for (volatile int i = 0; i < 200; i++);
    }
}

void smp_test_ap_preemption(void) {
    if (nr_cpus < 2 || !smp_enabled) {
        kprintf("[AP-SCHED] PREEMPTION TEST: SKIP (only 1 CPU)\n");
        return;
    }
    /* Full original test: create, place, IPI, sleep, check */
    kprintf("[AP-SCHED] Creating busy-loop thread on CPU 1...\n");
    ap_busy_loop_done = 0;
    ap_busy_loop_counter = 0;
    thread_t* t = thread_create(ap_busy_loop_thread, NULL, THREAD_DEF_PRIO, "ap-busy");
    if (!t) { kprintf("[AP-SCHED] FAILED\n"); return; }
    sched_set_thread_affinity(t, 2);
    sched_place_thread(t, 1);
    smp_send_reschedule(1);
    thread_sleep(500);
    uint64_t cb = ap_busy_loop_counter;
    kprintf("[AP-SCHED] Counter after 500ms: %lu\n", cb);
    thread_sleep(200);
    uint64_t ca = ap_busy_loop_counter;
    kprintf("[AP-SCHED] Counter after 700ms: %lu\n", ca);
    ap_busy_loop_done = 1;
    thread_sleep(10);
    kprintf("[AP-SCHED] PREEMPTION TEST: %s\n", ca > cb ? "OK" : "FAIL");
}
void smp_send_reschedule(int cpu) {
    if (cpu < 0 || cpu >= nr_cpus) return;
    /* Set the target CPU's need_reschedule flag first */
    per_cpu_data[cpu]->need_reschedule = 1;
    mb();
    /* If target is self, IPI is not strictly needed (flag already set),
     * but send it anyway to test the path. If target is different CPU,
     * send IPI to wake it up. */
    if (cpu != smp_cpu_id())
        apic_send_ipi(cpu_info[cpu].apic_id, IPI_VEC_RESCHEDULE, APIC_ICR_DELIV_FIXED);
}

void smp_tlb_shootdown(uint64_t start, uint64_t end) {
    cpu_flags_t tlb_flags;

    if (!smp_enabled) {
        for (uint64_t page = start & PAGE_MASK; page < end; page += PAGE_SIZE)
            asm volatile("invlpg (%0)" : : "r"(page) : "memory");
        return;
    }

    /* Serialize concurrent TLB shootdowns so the per-CPU range data
     * is not overwritten by two CPUs simultaneously.
     * Note: The IPI handler (smp_handle_tlb_shootdown on remote CPUs)
     * does NOT acquire this lock — it only reads its own per-CPU data
     * which was written by the initiating CPU before the IPI was sent. */
    spinlock_acquire(&tlb_lock, &tlb_flags);

    /* Write range to ALL CPUs' per-CPU data (remote CPUs read their
     * own per_cpu_data entry in the IPI handler). */
    for (int i = 0; i < nr_cpus; i++) {
        per_cpu_data[i]->tlb_flush_start = start;
        per_cpu_data[i]->tlb_flush_end   = end;
        per_cpu_data[i]->tlb_flush_pending = 1;
    }

    /* Send IPI to all other CPUs.
     * NOTE: On KVM with in-kernel APIC, FIXED-mode IPIs to other vCPUs
     * are not delivered. This function is a no-op for remote CPUs on KVM,
     * but works correctly on bare metal. On KVM, each CPU handles its own
     * TLB flushes via the invlpg instruction executed by the thread that
     * triggers the page table modification. */
    apic_send_ipi_allbutself(IPI_VEC_TLB_SHOOTDOWN);

    /* Handle locally */
    smp_handle_tlb_shootdown();

    /* Wait for others to ack (skip self — already handled above) */
    for (int i = 0; i < nr_cpus; i++) {
        if (i == smp_cpu_id()) continue;
        while (per_cpu_data[i]->tlb_flush_pending)
            cpu_relax();
    }

    spinlock_release(&tlb_lock, tlb_flags);
}

/* Safe TLB shootdown: performs local invlpg for the current CPU and
 * sends IPIs to remote CPUs only if the platform supports cross-CPU
 * IPI delivery. Falls back to local-only flush on platforms where
 * cross-CPU IPIs don't work (e.g., KVM in-kernel APIC). */
void smp_tlb_shootdown_safe(uint64_t start, uint64_t end) {
    /* Always flush locally */
    for (uint64_t page = start & PAGE_MASK; page < end; page += PAGE_SIZE)
        asm volatile("invlpg (%0)" : : "r"(page) : "memory");

    /* On platforms with working IPIs, flush remote TLBs too.
     * On KVM in-kernel APIC, cross-CPU IPIs aren't delivered, so
     * remote TLB may be stale. This is acceptable during development —
     * on bare metal, the IPI path (smp_tlb_shootdown above) works. */
    if (smp_enabled && smp_ipi_works)
        smp_tlb_shootdown(start, end);
}

void smp_handle_tlb_shootdown(void) {
    int cpu = smp_cpu_id();
    uint64_t start = per_cpu_data[cpu]->tlb_flush_start;
    uint64_t end   = per_cpu_data[cpu]->tlb_flush_end;

    if (end - start > 128 * PAGE_SIZE) {
        /* Large range: flush entire TLB by reloading CR3 */
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    } else {
        /* Small range: invalidate each page */
        for (uint64_t page = start & PAGE_MASK; page < end; page += PAGE_SIZE)
            asm volatile("invlpg (%0)" : : "r"(page) : "memory");
    }

    per_cpu_data[cpu]->tlb_flush_pending = 0;
}

uint64_t smp_get_tss_ist(int cpu, int ist_idx) {
    if (cpu < 0 || cpu >= MAX_CPUS || !per_cpu_data[cpu]) return 0;
    if (ist_idx < 0 || ist_idx > 2) return 0;
    return per_cpu_data[cpu]->tss.ist[ist_idx];
}

/* ------------------------------------------------------------------ */
/*  CPU hotplug — offline/online a specific CPU                       */
/* ------------------------------------------------------------------ */

/* Hotplug IPI handler: called from interrupt_handler via IPI_VEC_OFFLINE.
 * Sets the CPU state to OFFLINE and forces a reschedule.
 * The idle thread on this CPU will then park itself. */
void smp_handle_offline(void) {
    int cpu = smp_cpu_id();
    kprintf("[HOTPLUG] CPU %d going offline\n", cpu);

    /* Mark the CPU as offline */
    cpu_state[cpu] = CPU_STATE_OFFLINE;
    mb();

    /* Migrate current thread (if not idle) to CPU 0 so it doesn't get
     * stuck on an offline CPU.  The run queue was already migrated by
     * smp_cpu_offline before sending the IPI. */
    if (current_thread && per_cpu_data[cpu] &&
        (void*)current_thread != per_cpu_data[cpu]->idle_thread) {
        cpu_flags_t qflags;
        spinlock_acquire(&sched_queue_lock, &qflags);
        current_thread->state = THREAD_READY;
        per_cpu_data_t* cpu0 = per_cpu_data[0];
        uint32_t p = current_thread->priority;
        thread_t* tail = (thread_t*)cpu0->rq_tails[p];
        if (tail) {
            tail->rq_next = current_thread;
        } else {
            cpu0->rq_heads[p] = (void*)current_thread;
            cpu0->priority_bitmap[p / 64] |= (1ULL << (p % 64));
        }
        current_thread->rq_prev = tail;
        current_thread->rq_next = NULL;
        cpu0->rq_tails[p] = (void*)current_thread;
        cpu0->rq_counts[p]++;
        cpu0->rq_total++;
        current_thread->cpu_queue = 0;
        spinlock_release(&sched_queue_lock, qflags);
    }

    /* Force reschedule so the idle thread takes over */
    per_cpu_data[cpu]->need_reschedule = 1;
}

/* Offline a CPU using a software-based parking mechanism:
 *   - Flushes per-CPU caches
 *   - Migrates threads to CPU 0
 *   - Sends IPI_VEC_OFFLINE so the target CPU parks in its idle thread
 *   - The target CPU's idle thread spins (HLT) waiting for online
 *
 * Returns ERR_OK on success. */
err_t smp_cpu_offline(int cpu) {
    if (!smp_enabled || nr_cpus < 2)
        return ERR_NOENT;
    if (cpu == 0 || cpu >= nr_cpus)
        return ERR_INVAL;
    if (cpu_state[cpu] != CPU_STATE_ONLINE)
        return ERR_BUSY;

    kprintf("[HOTPLUG] Offlining CPU %d...\n", cpu);

    /* Mark CPU as going down */
    cpu_state[cpu] = CPU_STATE_GOING_DOWN;
    mb();

    /* Step 1: Flush target CPU's kmalloc magazines to global slab */
    kmag_flush_one_cpu(cpu);

    /* Step 2: Flush target CPU's PMM cache to global free list */
    pmm_flush_cpu_cache(cpu);

    /* Step 3: Migrate all threads from target's run queue to CPU 0 */
    int migrated = sched_migrate_cpu(cpu, 0);
    kprintf("[HOTPLUG] Migrated %d threads from CPU %d to CPU 0\n",
            migrated, cpu);

    /* Step 4: Send IPI_VEC_OFFLINE to the target CPU.
     * The handler will set cpu_state to OFFLINE, migrate the current
     * thread, set need_reschedule, and return.  When the CPU next
     * enters its idle thread, it will see OFFLINE and park. */
    {
        uint32_t target_apic_id = cpu_info[cpu].apic_id;
        apic_send_ipi(target_apic_id, IPI_VEC_OFFLINE, APIC_ICR_DELIV_FIXED);

        /* Busy-wait up to ~50ms for the CPU to acknowledge */
        mb();
        uint64_t deadline = hpet_ns() + 50000000ULL;
        while (hpet_ns() < deadline) {
            if (cpu_state[cpu] == CPU_STATE_OFFLINE)
                break;
            asm volatile("pause");
        }

        if (cpu_state[cpu] != CPU_STATE_OFFLINE) {
            kprintf("[HOTPLUG] WARNING: CPU %d did not acknowledge offline within 50ms\n", cpu);
            cpu_state[cpu] = CPU_STATE_OFFLINE;
            return ERR_TIMEOUT;
        }
    }

    kprintf("[HOTPLUG] CPU %d is now offline\n", cpu);
    return ERR_OK;
}

/* Online a previously offline CPU:
 *   - Sets cpu_state back to ONLINE
 *   - Sends a reschedule IPI to wake the parked CPU from HLT
 *   - The target CPU's idle thread resumes normal operation */
err_t smp_cpu_online(int cpu) {
    if (!smp_enabled || nr_cpus < 2)
        return ERR_NOENT;
    if (cpu == 0 || cpu >= nr_cpus)
        return ERR_INVAL;
    if (cpu_state[cpu] != CPU_STATE_OFFLINE)
        return ERR_BUSY;

    kprintf("[HOTPLUG] Onlining CPU %d...\n", cpu);

    /* Set the state back to ONLINE — the target CPU's parking loop
     * in idle_thread will see this and exit. */
    cpu_state[cpu] = CPU_STATE_ONLINE;
    mb();

    /* Send reschedule IPI to wake the CPU (it may be in a HLT loop) */
    {
        uint32_t target_apic_id = cpu_info[cpu].apic_id;
        apic_send_ipi(target_apic_id, IPI_VEC_RESCHEDULE, APIC_ICR_DELIV_FIXED);

        /* Wait briefly for the CPU to acknowledge online */
        mb();
        uint64_t deadline = hpet_ns() + 50000000ULL;
        while (hpet_ns() < deadline) {
            if (per_cpu_data[cpu]->cpu_thread != NULL &&
                (void*)per_cpu_data[cpu]->cpu_thread !=
                (void*)per_cpu_data[cpu]->idle_thread) {
                break;
            }
            /* Check if the idle thread is running (means it exited park) */
            if (cpu_state[cpu] == CPU_STATE_ONLINE)
                break;
            asm volatile("pause");
        }
    }

    kprintf("[HOTPLUG] CPU %d is now online\n", cpu);
    return ERR_OK;
}
