#include "kernel.h"
#include "watchdog.h"
#include "hal.h"
#include "pmm.h"
#include "sched.h"
#include "eventbus.h"
#include "apic.h"
#include "smp.h"
#include "barrier.h"

#define MAX_WATCHDOG_LAYERS 16

static watchdog_layer_t layers[MAX_WATCHDOG_LAYERS];
static int num_layers = 0;
static volatile int watchdog_running = 0;
static volatile uint64_t last_watchdog_tick = 0;

static watchdog_health_t hal_health_check(char* reason, size_t len) {
    uint64_t ticks = hal_timer_get_ticks();
    if (ticks == last_watchdog_tick && ticks > 10) {
        kstrncpy(reason, "Timer appears stalled", len);
        return WATCHDOG_FAILED;
    }

    uint64_t free_pct = pmm_total_pages() ?
        pmm_free_pages_count() * 100 / pmm_total_pages() : 0;
    if (free_pct < 5) {
        kstrncpy(reason, "Critically low memory", len);
        return WATCHDOG_DEGRADED;
    }

    last_watchdog_tick = ticks;
    kstrncpy(reason, "OK", len);
    return WATCHDOG_OK;
}

static watchdog_health_t sched_health_check(char* reason, size_t len) {
    if (!sched_running) {
        kstrncpy(reason, "Scheduler not running", len);
        return WATCHDOG_FAILED;
    }
    if (!current_thread) {
        kstrncpy(reason, "No current thread", len);
        return WATCHDOG_FAILED;
    }
    kstrncpy(reason, "OK", len);
    return WATCHDOG_OK;
}

static watchdog_health_t pmm_health_check(char* reason, size_t len) {
    uint64_t free = pmm_free_pages_count();
    uint64_t total = pmm_total_pages();

    if (total == 0) {
        kstrncpy(reason, "PMM not initialized", len);
        return WATCHDOG_FAILED;
    }

    if (free == 0) {
        kstrncpy(reason, "Out of memory", len);
        return WATCHDOG_FAILED;
    }

    if (free * 100 / total < 10) {
        kstrncpy(reason, "Low memory warning", len);
        return WATCHDOG_DEGRADED;
    }

    kstrncpy(reason, "OK", len);
    return WATCHDOG_OK;
}

void watchdog_register_layer(int id, const char* name,
                             watchdog_health_t (*check)(char*, size_t)) {
    if (num_layers >= MAX_WATCHDOG_LAYERS) return;
    layers[num_layers].layer_id = id;
    layers[num_layers].name = name;
    layers[num_layers].check = check;
    layers[num_layers].consecutive_failures = 0;
    layers[num_layers].total_failures = 0;
    num_layers++;
}

#define WATCHDOG_BUF_SIZE 512
static char watchdog_buf[WATCHDOG_BUF_SIZE];
static volatile int watchdog_buf_pos = 0;

static void watchdog_buf_write(const char* s) {
    cpu_flags_t flags = hal_save_irq();
    int pos = watchdog_buf_pos;
    while (*s && pos < WATCHDOG_BUF_SIZE - 1)
        watchdog_buf[pos++] = *s++;
    watchdog_buf[pos] = '\0';
    watchdog_buf_pos = pos;
    hal_restore_irq(flags);
}

void watchdog_flush(void) {
    cpu_flags_t flags = hal_save_irq();
    int pos = watchdog_buf_pos;
    if (pos == 0) { hal_restore_irq(flags); return; }
    watchdog_buf_pos = 0;
    hal_restore_irq(flags);
    kputs(watchdog_buf);
}

void watchdog_run(void) {
    if (!watchdog_running) return;

    char reason[128];

    for (int i = 0; i < num_layers; i++) {
        reason[0] = '\0';
        watchdog_health_t health = layers[i].check(reason, sizeof(reason));

        if (health == WATCHDOG_FAILED) {
            layers[i].consecutive_failures++;
            layers[i].total_failures++;

            eventbus_publish(EV_WATCHDOG, (uint64_t)layers[i].layer_id,
                           (uint64_t)health, (uint64_t)layers[i].total_failures, 0);

            char tmp[128];
            kstrncpy(tmp, reason, sizeof(tmp) - 1);
            watchdog_buf_write("[WATCHDOG] Layer ");
            watchdog_buf_write(layers[i].name);
            watchdog_buf_write(" FAILED: ");
            watchdog_buf_write(tmp);
            watchdog_buf_write("\n");

            if (layers[i].consecutive_failures >= WATCHDOG_MAX_FAILURES) {
                watchdog_buf_write("[WATCHDOG] Escalating: Layer ");
                watchdog_buf_write(layers[i].name);
                watchdog_buf_write(" exceeded max failures. PANIC!\n");
                watchdog_flush();
                kpanic("Watchdog layer failure");
            }
        } else if (health == WATCHDOG_DEGRADED) {
            layers[i].consecutive_failures = 0;
            char tmp[128];
            kstrncpy(tmp, reason, sizeof(tmp) - 1);
            watchdog_buf_write("[WATCHDOG] Layer ");
            watchdog_buf_write(layers[i].name);
            watchdog_buf_write(" DEGRADED: ");
            watchdog_buf_write(tmp);
            watchdog_buf_write("\n");
        } else {
            layers[i].consecutive_failures = 0;
        }
    }
}

err_t watchdog_init(void) {
    kmemset(layers, 0, sizeof(layers));
    num_layers = 0;

    watchdog_register_layer(1, "HAL", hal_health_check);
    watchdog_register_layer(2, "Scheduler", sched_health_check);
    watchdog_register_layer(4, "Memory", pmm_health_check);

    watchdog_running = 1;

    kprintf("[WATCHDOG] Cross-cutting initialized: monitoring %d layers\n",
            num_layers);
    return ERR_OK;
}

void watchdog_timer_handler(int_frame_t* frame, void* data) {
    (void)frame;
    (void)data;

    /* sched_timer_tick is now called directly from the timer ISR in hal.c */

    static uint64_t last_check = 0;
    uint64_t now = hal_timer_get_ticks();
    if (now - last_check >= 1000) {
        last_check = now;
        watchdog_run();
    }
}

/* ============================================================
 * NMI lockup detector
 *
 * Uses a per-CPU NMI count that is incremented whenever the CPU
 * receives an NMI (vector 2).  NMIs are broadcast to all other
 * CPUs from the BSP timer tick.  The NMI handler also records
 * the interrupted RIP/RSP and detects when a CPU has not moved
 * for several consecutive NMI samples — indicating a hard lockup
 * (interrupts disabled, stuck in a tight loop, etc.).
 * ============================================================ */

/* Interval in timer ticks between NMI broadcasts */
#define WDT_NMI_INTERVAL_TICKS  10       /* every 10ms at 1000 Hz */

/* Check NMI sample every N NMI events */
#define WDT_NMI_SAMPLE_RATE     3

/* Stuck threshold: consecutive samples with identical RIP/RSP */
#define WDT_STUCK_THRESHOLD     2

static volatile uint64_t wdt_nmi_count[MAX_CPUS];
static uint64_t wdt_last_rip[MAX_CPUS];
static uint64_t wdt_last_rsp[MAX_CPUS];
static volatile int wdt_stuck_count[MAX_CPUS];
static volatile int wdt_stuck[MAX_CPUS];

void watchdog_nmi_handler(int_frame_t* frame) {
    int cpu = smp_cpu_id();
    uint64_t count = __sync_fetch_and_add(&wdt_nmi_count[cpu], 1);

    /* Skip stuck detection for idle threads — the idle loop intentionally
     * stays at the same RIP (HLT instruction) for extended periods and is
     * NOT a lockup.  Read cpu_thread directly from per-CPU data rather than
     * using the current_thread macro, which may not resolve correctly from
     * NMI context on non-BSP CPUs. */
    if (cpu >= 0 && cpu < nr_cpus && per_cpu_data[cpu]) {
        thread_t* cur = (thread_t*)per_cpu_data[cpu]->cpu_thread;
        thread_t* idle = (thread_t*)per_cpu_data[cpu]->idle_thread;
        if (cur == idle || cur == NULL || idle == NULL)
            return;
    }

    if ((count % WDT_NMI_SAMPLE_RATE) == 0) {
        uint64_t rip = frame->rip;
        uint64_t rsp = frame->rsp;

        if (rip == wdt_last_rip[cpu] && rsp == wdt_last_rsp[cpu]) {
            wdt_stuck_count[cpu]++;
            if (wdt_stuck_count[cpu] >= WDT_STUCK_THRESHOLD) {
                if (!wdt_stuck[cpu]) {
                    kprintf("[WDT] CPU %d stuck at rip=%lx rsp=%lx\n",
                            cpu, rip, rsp);
                }
                wdt_stuck[cpu] = 1;
            }
        } else {
            wdt_stuck_count[cpu] = 0;
            wdt_stuck[cpu] = 0;
        }

        wdt_last_rip[cpu] = rip;
        wdt_last_rsp[cpu] = rsp;
    }
}

void watchdog_send_nmis_tick(void) {
    /* Only the BSP sends NMI IPIs to other CPUs */
    if (!smp_enabled || nr_cpus < 2 || smp_cpu_id() != 0)
        return;

    static uint64_t tick_counter = 0;
    tick_counter++;
    if ((tick_counter % WDT_NMI_INTERVAL_TICKS) != 0)
        return;

    /* Broadcast NMI to all other CPUs via the "All Except Self" shorthand.
     * NMI delivery mode bypasses the IF flag, reaching CPUs with interrupts
     * disabled.  The vector field in ICR is ignored for NMI delivery. */
    apic_send_nmi_allbutself();
}

int watchdog_get_stuck(int cpu) {
    if (cpu < 0 || cpu >= nr_cpus) return 0;
    return wdt_stuck[cpu];
}

void watchdog_clear_stuck(void) {
    int cpu = smp_cpu_id();
    if (cpu >= 0 && cpu < MAX_CPUS) {
        wdt_stuck_count[cpu] = 0;
        wdt_stuck[cpu] = 0;
    }
}
