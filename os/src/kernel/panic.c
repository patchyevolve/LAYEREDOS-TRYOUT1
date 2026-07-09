#include "kernel.h"
#include "panic.h"
#include "hal.h"
#include "block.h"

extern volatile uint64_t tsc_khz;

void emergency_sync(void) {
    if (block_try_sync()) {
        kprintf("[PANIC] emergency_sync: block cache flushed\n");
    } else {
        kprintf("[PANIC] emergency_sync: cache_lock contended, skipping\n");
    }
}

void panic_reboot(void) {
    /* Use RDTSC for timeout — works even with interrupts disabled.
     * Capture rdtsc() ONCE per loop iteration to avoid the "after" value
     * wrapping past deadline and producing a bogus remaining-seconds. */
    uint64_t now = rdtsc();
    uint64_t deadline;
    if (tsc_khz > 0) {
        deadline = now + (uint64_t)5000 * tsc_khz;
    } else {
        /* Approximate: assume ~2 GHz, loop ~10 billion iterations */
        deadline = now + (uint64_t)10000000000ULL;
    }
    uint64_t sec_tsc = tsc_khz > 0 ? (uint64_t)1000 * tsc_khz : (uint64_t)2000000000ULL;

    kprintf("\n[PANIC] Rebooting in 5 seconds...\n");
    uint64_t last_remaining = 6;
    for (;;) {
        now = rdtsc();
        if (now >= deadline) break;
        uint64_t remaining = (deadline - now) / sec_tsc;
        if (remaining != last_remaining) {
            kprintf("\r[PANIC] Reboot in %lu seconds...  ", (unsigned long)remaining);
            last_remaining = remaining;
        }
        asm volatile("pause");
    }
    kprintf("\n[PANIC] Rebooting now.\n");
    hal_reboot();
}
