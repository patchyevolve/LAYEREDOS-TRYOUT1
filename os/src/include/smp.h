#ifndef SMP_H
#define SMP_H

#include "types.h"
#include "acpi.h"

/* SMP mode flags */
#define SMP_ENABLED     1
#define SMP_NEED_TLB    2

extern int smp_enabled;
extern int smp_flags;
extern int smp_ipi_works;   /* 0 = cross-CPU IPIs not reliable (KVM), 1 = working */
extern volatile int ap_ready_count;

/* CPU state tracking for hotplug */
typedef enum {
    CPU_STATE_OFFLINE = 0,
    CPU_STATE_ONLINE,
    CPU_STATE_GOING_DOWN
} cpu_state_t;
extern cpu_state_t cpu_state[MAX_CPUS];
extern volatile int ap_ipi_test_counter;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;

#define GDT_ENTRIES 8

/* Per-CPU data (must be cache-line aligned) */
typedef struct {
    /* Thread management */
    void*    cpu_thread;        /* thread_t* running on this CPU */
    void*    idle_thread;       /* idle thread for this CPU */

    /* Run queue (embedded to avoid extra allocation) */
    void*    rq_heads[256];     /* run_queue_t head per priority (thread_t*) */
    void*    rq_tails[256];     /* run_queue_t tail per priority (thread_t*) */
    uint32_t rq_counts[256];    /* count per priority */
    uint64_t priority_bitmap[4];
    uint32_t rq_total;          /* total threads in all queues */

    /* Scheduling */
    volatile int need_reschedule;
    volatile uint64_t idle_wake_hint;
    int         cpu_id;
    int         node_id;         /* NUMA node for this CPU (-1 if unknown) */
    uint64_t    cpu_khz;         /* calibrated APIC frequency */
    uint64_t    irq_count;
    uint64_t    context_switches;
    uint64_t    aging_counter;
    uint64_t    balance_counter;

    /* TLB shootdown */
    volatile int  tlb_flush_pending;
    volatile uint64_t tlb_flush_start;
    volatile uint64_t tlb_flush_end;

    /* Per-CPU GDT and TSS */
    uint64_t gdt[GDT_ENTRIES] __attribute__((aligned(8)));
    tss64_t tss;

    /* Per-CPU stacks */
    uint8_t ist_stack0[8192] __attribute__((aligned(16)));  /* IST1: #DF */
    uint8_t ist_stack1[8192] __attribute__((aligned(16)));  /* IST2: #PF */
    uint8_t user_stack0[16384] __attribute__((aligned(16)));

    /* RDTSC-based periodic check timestamps (per-CPU to avoid shared
     * static variable races on TCG where vCPUs can interleave). */
    uint64_t last_check_rdtsc;
    uint64_t last_balance_rdtsc;

    /* Thread being retired on this CPU — set by thread_exit() before
     * switch_context, cleared by sched_finalize_retiring() after
     * switch_context returns.  Prevents sched_reap_zombies() from
     * freeing the stack while the exiting CPU is still using it. */
    volatile uint64_t retiring_thread;   /* kernel-virtual addr of thread_t */

    /* Per-CPU PMM cache (atomic flag avoids sync.h include) */
    volatile uint64_t pmm_cache_lock;    /* 0 = free, 1 = held */
    uint64_t          pmm_cache[32];     /* physical addresses of cached pages */
    int               pmm_cache_count;   /* number of pages in cache */

    /* Padding to cache-line boundary */
    uint8_t  pad[128];
} __attribute__((aligned(64))) per_cpu_data_t;

/* Per-CPU offsets array */
extern uint64_t __per_cpu_offset[MAX_CPUS];
extern per_cpu_data_t* per_cpu_data[MAX_CPUS];

/* CPU identification */
int smp_cpu_id(void);
int smp_nr_cpus(void);
void smp_init(void);
void smp_init_aps(void);

/* Per-CPU accessors */
static inline per_cpu_data_t* smp_get_per_cpu(int cpu) {
    return per_cpu_data[cpu];
}

static inline per_cpu_data_t* smp_this_cpu(void) {
    return per_cpu_data[smp_cpu_id()];
}

/* IPI types */
#define IPI_VEC_RESCHEDULE   0x41
#define IPI_VEC_TLB_SHOOTDOWN 0x42
#define IPI_VEC_PANIC        0x43
#define IPI_VEC_OFFLINE      0x44

/* IPI helpers */
void smp_send_reschedule(int cpu);
void smp_test_ipi(void);
void smp_test_cross_cpu_ipi(void);
void smp_test_ap_preemption(void);

/* TLB shootdown */
void smp_tlb_shootdown(uint64_t start, uint64_t end);
void smp_tlb_shootdown_safe(uint64_t start, uint64_t end);
void smp_handle_tlb_shootdown(void);

/* AP entry point (called by trampoline on APs) */
void ap_entry(per_cpu_data_t* pcp);

/* Hotplug IPI handler — marks CPU as offline and parks it */
void smp_handle_offline(void);

/* CPU hotplug */
err_t smp_cpu_offline(int cpu);
err_t smp_cpu_online(int cpu);

/* TSS diagnostic */
uint64_t smp_get_tss_ist(int cpu, int ist_idx);

/* Trampoline symbols (physical addresses in .trampoline section) */
extern char _trampoline_start[], _trampoline_end[];

/* AP stack size */
#define AP_STACK_SIZE 16384

#endif /* SMP_H */
