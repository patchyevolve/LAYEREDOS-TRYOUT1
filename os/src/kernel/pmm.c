#include "kernel.h"
#include "pmm.h"
#include "hal.h"
#include "eventbus.h"
#include "sched.h"
#include "process.h"
#include "work.h"
#include "kmalloc.h"
#include "sync.h"
#include "acpi.h"

#define PMM_PERCPU_CACHE_SIZE 32

typedef struct free_page {
    struct free_page* next;
} free_page_t;

/* Global free list (protected by pmm_global_lock) */
static free_page_t*  free_list = NULL;
static uint64_t      total_page_count = 0;
static uint64_t      global_free_count = 0;
static uint64_t      total_memory = 0;

static uint64_t      bitmap_base = 0;
static uint64_t      bitmap_pages = 0;
static uint8_t*      used_bitmap;

static spinlock_t    pmm_global_lock;
static work_item_t   oom_work_item;
static volatile int  oom_scheduled = 0;

/* Page ownership tracking: records which thread owns each page.
 * 0 = unowned (free).  Used for double-allocation detection.
 * Array allocated at 0x10000 + bitmap_pages * PAGE_SIZE. */
static uint64_t*     page_owner;
#define PAGE_OWNER_UNSET 0

/* Per-CPU caches: each CPU has a small stash of pages.
 * Pages in per-CPU cache have bitmap SET (considered in-use by the PMM
 * system).  When flushed back to the global list the bitmap is cleared.
 * pmm_free_pages_count() reports the sum across all CPUs + global list.
 */
static uint64_t      cpu_cache[MAX_CPUS][PMM_PERCPU_CACHE_SIZE];
static int           cpu_cache_count[MAX_CPUS];
static spinlock_t    cpu_cache_lock[MAX_CPUS];

/* ------------------------------------------------------------------ */
/*  OOM handling                                                      */
/* ------------------------------------------------------------------ */
static void pmm_oom_kill_worker(void* arg) {
    (void)arg;
    kprintf("[OOM] Out of memory! Killing current process...\n");
    eventbus_publish(EV_OOM_KILL, 0, 0, 0, 0);
    if (current_thread && current_thread->proc) {
        process_exit(current_thread->proc, -12);
        thread_exit(-12);
    }
    oom_scheduled = 0;
}

static void pmm_oom_kill(void) {
    if (!oom_scheduled) {
        kmalloc_compact();
        oom_scheduled = 1;
        oom_work_item.func = pmm_oom_kill_worker;
        oom_work_item.data = NULL;
        work_queue_schedule(&system_wq, &oom_work_item);
    }
}

/* ------------------------------------------------------------------ */
/*  Bitmap helpers                                                    */
/* ------------------------------------------------------------------ */
static void bitmap_set(uint64_t page_idx) {
    used_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}
static void bitmap_clear(uint64_t page_idx) {
    used_bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
}
static int bitmap_test(uint64_t page_idx) {
    return (used_bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}

/* Page owner helpers */
#define PAGE_OWNER_UNSET   0
#define PAGE_OWNER_CACHE   ((uint64_t)-1)

static void owner_set(uint64_t idx, uint64_t owner) {
    if (idx >= total_page_count || !page_owner) return;
    uint64_t old = page_owner[idx];
    page_owner[idx] = owner;
    /* Double-allocation: only crash if both old and new are distinct thread pointers */
    if (old != PAGE_OWNER_UNSET && old != PAGE_OWNER_CACHE &&
        owner != PAGE_OWNER_UNSET && owner != PAGE_OWNER_CACHE &&
        old != owner) {
        kprintf("[PMM] CRASH: page %lx (idx %lu) re-alloc: old_owner=%lx new_owner=%lx\n",
                idx * PAGE_SIZE, idx, old, owner);
        for (;;) asm("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/*  Per-CPU cache helpers                                             */
/* ------------------------------------------------------------------ */
static int current_cpu(void) {
    return smp_cpu_id();
}

/* Try to pop a page from the current CPU's cache.  Returns 0 if empty.
 * Page ownership is updated inside the per-CPU lock so another CPU's
 * cache_try_push that sees the new cache state also sees the updated owner. */
static uint64_t cache_try_pop(void) {
    int cpu = current_cpu();
    cpu_flags_t flags;
    spinlock_acquire(&cpu_cache_lock[cpu], &flags);
    int n = cpu_cache_count[cpu];
    if (n <= 0) {
        spinlock_release(&cpu_cache_lock[cpu], flags);
        return 0;
    }
    n--;
    uint64_t phys = cpu_cache[cpu][n];
    cpu_cache_count[cpu] = n;

    /* Integrity check: pages in per-CPU cache must have bitmap SET */
    uint64_t idx = phys / PAGE_SIZE;
    if (idx < total_page_count && !bitmap_test(idx)) {
            kprintf("[PMM] CRASH: cache_try_pop page %lx (idx %lu) has bitmap CLEAR!\n",
                    phys, idx);
            spinlock_release(&cpu_cache_lock[cpu], flags);
            for (;;) asm("cli; hlt");
        }
        /* Atomically claim the page from CACHE state.  cmpxchg prevents a
         * concurrent cache_try_push from also claiming this page between
         * our read and write (TOCTOU between per-CPU locks). */
        if (page_owner && idx < total_page_count) {
            uint64_t expected = PAGE_OWNER_CACHE;
            if (!__sync_bool_compare_and_swap(&page_owner[idx], expected,
                                              (uint64_t)current_thread)) {
                /* Page was claimed by another CPU in between our pop and
                 * this cmpxchg.  Don't push it back — it's already in use.
                 * The slot is outside the active cache range (count was
                 * already decremented) and will be overwritten on the next
                 * push. */
                spinlock_release(&cpu_cache_lock[cpu], flags);
                return 0;
            }
            /* Verify the cmpxchg actually took effect.  On real x86 the
             * lock cmpxchgq is atomic; on buggy QEMU TCG with MTTCG the
             * store may be silently dropped under rare timing conditions. */
            if (page_owner[idx] != (uint64_t)current_thread) {
                kprintf("[PMM] CRASH: cmpxchg verify failed pop page %lx idx %lu "
                        "expected=%lx got=%lx\n",
                        phys, idx, (uint64_t)current_thread, page_owner[idx]);
                spinlock_release(&cpu_cache_lock[cpu], flags);
                for (;;) asm("cli; hlt");
            }
        }
    spinlock_release(&cpu_cache_lock[cpu], flags);
    return phys;
}

/* Try to push a page to the current CPU's cache.  Returns 1 on success,
 * 0 if the cache is full (caller must flush).
 * Checks for duplicates in the local cache (same-CPU double-free).
 * Only cross-CPU invariant enforced: page_owner must not be CACHE
 * (which would mean the page is already in another CPU's cache). */
static int cache_try_push(uint64_t phys) {
    int cpu = current_cpu();
    cpu_flags_t flags;
    spinlock_acquire(&cpu_cache_lock[cpu], &flags);
    int n = cpu_cache_count[cpu];
    /* Check for duplicate in local cache */
    for (int i = 0; i < n; i++) {
        if (cpu_cache[cpu][i] == phys) {
            kprintf("[PMM] WARN: double-free page %lx into CPU %d cache slot %d\n",
                    phys, cpu, i);
            spinlock_release(&cpu_cache_lock[cpu], flags);
            return 1;
        }
    }
    if (n >= PMM_PERCPU_CACHE_SIZE) {
        spinlock_release(&cpu_cache_lock[cpu], flags);
        return 0;
    }

    /* Only invariant: page must not already be in another CPU's cache.
     * Use cmpxchg to atomically claim the page, preventing TOCTOU with
     * a concurrent cache_try_pop on another CPU.
     * NOTE: we do NOT check old against current_thread — pages are
     * legitimately freed by a different thread than the allocator
     * (e.g. sched_reap_zombies freeing another thread's stack/TCB). */
    uint64_t idx = phys / PAGE_SIZE;
    if (page_owner && idx < total_page_count) {
        uint64_t old = page_owner[idx];
        if (old == PAGE_OWNER_CACHE) {
            kprintf("[PMM] CRASH: double-free page %lx (idx %lu) already in cache!\n",
                    phys, idx);
            spinlock_release(&cpu_cache_lock[cpu], flags);
            for (;;) asm("cli; hlt");
        }
        /* Atomically set to CACHE, but only if it's still the value we
         * just checked.  If another CPU's cache_try_pop or cache_try_push
         * changed it in between, the cmpxchg fails and we must not push. */
        if (!__sync_bool_compare_and_swap(&page_owner[idx], old, PAGE_OWNER_CACHE)) {
            /* Another CPU pushed or popped this page concurrently.
             * Our cmpxchg failed, so we must NOT push.  The slow path
             * will detect this via the bitmap check. */
            spinlock_release(&cpu_cache_lock[cpu], flags);
            return 0;
        }
        /* Verify cmpxchg took effect */
        if (page_owner[idx] != PAGE_OWNER_CACHE) {
            kprintf("[PMM] CRASH: cmpxchg verify failed push page %lx idx %lu "
                    "expected=CACHE got=%lx\n",
                    phys, idx, page_owner[idx]);
            spinlock_release(&cpu_cache_lock[cpu], flags);
            for (;;) asm("cli; hlt");
        }
    }

    cpu_cache[cpu][n] = phys;
    cpu_cache_count[cpu] = n + 1;
    spinlock_release(&cpu_cache_lock[cpu], flags);
    return 1;
}

/* Flush the oldest N/2 pages from the current CPU's cache back to the
 * global free list.  Must hold pmm_global_lock. */
static void cache_flush_half(void) {
    int cpu = current_cpu();
    cpu_flags_t flags;
    spinlock_acquire(&cpu_cache_lock[cpu], &flags);
    int n = cpu_cache_count[cpu];
    int keep = n / 2;
    for (int i = keep; i < n; i++) {
        uint64_t flush_phys = cpu_cache[cpu][i];
        uint64_t fidx = flush_phys / PAGE_SIZE;
        if (fidx < total_page_count) {
            bitmap_clear(fidx);
            owner_set(fidx, PAGE_OWNER_UNSET);
            global_free_count++;
            free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(flush_phys);
            fp->next = free_list;
            free_list = fp;
        }
    }
    cpu_cache_count[cpu] = keep;
    spinlock_release(&cpu_cache_lock[cpu], flags);
}

/* Flush ALL pages from a specific CPU's cache back to the global free list.
 * Used during CPU hotplug offlining. */
void pmm_flush_cpu_cache(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    cpu_flags_t flags;
    spinlock_acquire(&pmm_global_lock, &flags);
    cpu_flags_t lflags;
    spinlock_acquire(&cpu_cache_lock[cpu], &lflags);
    int n = cpu_cache_count[cpu];
    for (int i = 0; i < n; i++) {
        uint64_t flush_phys = cpu_cache[cpu][i];
        uint64_t fidx = flush_phys / PAGE_SIZE;
        if (fidx < total_page_count) {
            bitmap_clear(fidx);
            owner_set(fidx, PAGE_OWNER_UNSET);
            global_free_count++;
            free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(flush_phys);
            fp->next = free_list;
            free_list = fp;
        }
    }
    cpu_cache_count[cpu] = 0;
    spinlock_release(&cpu_cache_lock[cpu], lflags);
    spinlock_release(&pmm_global_lock, flags);
}

/* Refill the current CPU's cache from the global free list (up to
 * capacity).  Must hold pmm_global_lock.  Appends to existing cache
 * entries rather than resetting — this avoids losing pages that were
 * pushed between a cache_try_pop() miss and the acquisition of
 * pmm_global_lock. */
static void cache_refill(void) {
    int cpu = current_cpu();
    cpu_flags_t flags;
    spinlock_acquire(&cpu_cache_lock[cpu], &flags);
    int count = cpu_cache_count[cpu];
    int cap = PMM_PERCPU_CACHE_SIZE;
    while (free_list && count < cap) {
        free_page_t* page = free_list;
        uint64_t phys = VIRT_TO_PHYS(page);
        uint64_t idx = phys / PAGE_SIZE;
        if ((phys & 0xFFF) || idx >= total_page_count) break;
        free_list = page->next;
        global_free_count--;
        bitmap_set(idx);
        owner_set(idx, PAGE_OWNER_CACHE);
        cpu_cache[cpu][count++] = phys;
    }
    cpu_cache_count[cpu] = count;
    spinlock_release(&cpu_cache_lock[cpu], flags);
}

/* Steal up to PMM_PERCPU_CACHE_SIZE/2 pages from another CPU's cache.
 * Must hold pmm_global_lock (serializes stealers).
 * Uses try_acquire for the victim's cache lock — if the victim is
 * concurrently doing a cache operation while holding pmm_global_lock
 * would create an ABBA deadlock (stealer: pmm_global→victim_lock;
 * victim: victim_lock→pmm_global).  try_acquire breaks the cycle:
 * if the victim's lock is busy, we skip and try a different victim. */
static int cache_steal(int victim_cpu) {
    if (victim_cpu == current_cpu()) return 0;
    int my_cpu = current_cpu();
    cpu_flags_t vflags, mflags;
    if (!spinlock_try_acquire(&cpu_cache_lock[victim_cpu], &vflags))
        return 0;
    int n = cpu_cache_count[victim_cpu];
    int steal = n / 2;
    if (steal <= 0) {
        spinlock_release(&cpu_cache_lock[victim_cpu], vflags);
        return 0;
    }
    int my_n = cpu_cache_count[my_cpu];
    int room = PMM_PERCPU_CACHE_SIZE - my_n;
    if (room > steal) room = steal;

    /* Must acquire our own lock with try too — the caller already holds
     * pmm_global_lock, and a concurrent cache_steal on another CPU that
     * has our lock and wants the victim's lock would ABBA deadlock if we
     * did a blocking acquire. */
    if (!spinlock_try_acquire(&cpu_cache_lock[my_cpu], &mflags)) {
        cpu_cache_count[victim_cpu] = n;  /* restore victim count */
        spinlock_release(&cpu_cache_lock[victim_cpu], vflags);
        return 0;
    }

    cpu_cache_count[victim_cpu] = n - steal;
    for (int i = 0; i < room; i++) {
        uint64_t sp = cpu_cache[victim_cpu][n - steal + i];
        cpu_cache[my_cpu][my_n + i] = sp;
        uint64_t sidx = sp / PAGE_SIZE;
        if (page_owner && sidx < total_page_count)
            page_owner[sidx] = PAGE_OWNER_CACHE;
    }
    cpu_cache_count[my_cpu] = my_n + room;
    spinlock_release(&cpu_cache_lock[my_cpu], mflags);
    spinlock_release(&cpu_cache_lock[victim_cpu], vflags);
    return room;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */
uint64_t pmm_total_pages(void) { return total_page_count; }

uint64_t pmm_free_pages_count(void) {
    uint64_t total = global_free_count;
    for (int i = 0; i < MAX_CPUS; i++) {
        total += cpu_cache_count[i];
    }
    return total;
}

uint64_t pmm_used_pages(void) {
    uint64_t free_sum = global_free_count;
    for (int i = 0; i < MAX_CPUS; i++) {
        free_sum += cpu_cache_count[i];
    }
    return total_page_count - free_sum;
}

void pmm_mark_region_used(uint64_t start, uint64_t end) {
    uint64_t sp = start / PAGE_SIZE;
    uint64_t ep = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = sp; i < ep && i < total_page_count; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            global_free_count--;
        }
    }
}

uint64_t pmm_alloc_page(void) {
    /* Fast path: try per-CPU cache */
    uint64_t phys = cache_try_pop();
    if (phys) {
        page_zero(phys);
        return phys;
    }

    /* Slow path: refill from global */
    cpu_flags_t flags;
    spinlock_acquire(&pmm_global_lock, &flags);

    if (!free_list) {
        /* Try stealing from other CPUs */
        int nr = smp_nr_cpus();
        for (int cpu = 0; cpu < nr; cpu++) {
            if (cache_steal(cpu)) {
                spinlock_release(&pmm_global_lock, flags);
                phys = cache_try_pop();
                if (phys) {
                    page_zero(phys);
                    return phys;
                }
                spinlock_acquire(&pmm_global_lock, &flags);
            }
        }
        spinlock_release(&pmm_global_lock, flags);
        pmm_oom_kill();
        return 0;
    }

    /* Pop one page from global free list */
    free_page_t* page = free_list;
    phys = VIRT_TO_PHYS(page);
    uint64_t idx = phys / PAGE_SIZE;

    if (phys & 0xFFF) {
        kprintf("[PMM] CRASH: free-list corruption! phys=%lx (misaligned)\n", phys);
        for (;;) asm("cli; hlt");
    }
    if (idx >= total_page_count) {
        kprintf("[PMM] CRASH: free-list corruption! phys=%lx idx=%lu >= total=%lu\n",
                phys, idx, total_page_count);
        for (;;) asm("cli; hlt");
    }

    free_list = page->next;
    global_free_count--;

    if (bitmap_test(idx)) {
        kprintf("[PMM] CRASH: page %lx (idx %lu) DOUBLE-ALLOCATED! free_list=%lx\n",
                phys, idx, (uint64_t)page->next);
        kprintf("[PMM] global_free_count=%lu total=%lu\n", global_free_count, total_page_count);
        for (;;) asm("cli; hlt");
    }
    bitmap_set(idx);
    owner_set(idx, (uint64_t)current_thread);

    /* Batch-refill per-CPU cache while we hold the global lock */
    cache_refill();

    spinlock_release(&pmm_global_lock, flags);
    page_zero(phys);
    return phys;
}

uint64_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;

    cpu_flags_t flags;
    spinlock_acquire(&pmm_global_lock, &flags);

    /* Flush all per-CPU caches so the bitmap reflects true free pages */
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_flags_t lflags;
        spinlock_acquire(&cpu_cache_lock[cpu], &lflags);
        int n = cpu_cache_count[cpu];
        for (int i = 0; i < n; i++) {
            uint64_t fphys = cpu_cache[cpu][i];
            uint64_t fidx = fphys / PAGE_SIZE;
            if (fidx < total_page_count) {
                bitmap_clear(fidx);
                owner_set(fidx, PAGE_OWNER_UNSET);
                global_free_count++;
                free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(fphys);
                fp->next = free_list;
                free_list = fp;
            }
        }
        cpu_cache_count[cpu] = 0;
        spinlock_release(&cpu_cache_lock[cpu], lflags);
    }

    /* Scan bitmap for contiguous free region */
    uint64_t first = 0;
    uint32_t found = 0;
    for (uint64_t i = 0; i < total_page_count && found < count; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) first = i;
            found++;
        } else {
            found = 0;
        }
    }

    if (found < count) { spinlock_release(&pmm_global_lock, flags); return 0; }

    for (uint32_t j = 0; j < count; j++) {
        bitmap_set(first + j);
        global_free_count--;
        owner_set(first + j, (uint64_t)current_thread);
    }

    /* Single O(N) pass to remove allocated pages from free list */
    free_page_t** pp = &free_list;
    while (*pp) {
        free_page_t* cur = *pp;
        uint64_t pa = VIRT_TO_PHYS(cur);
        uint64_t pidx = pa / PAGE_SIZE;
        if (pidx >= first && pidx < first + count) {
            *pp = cur->next;
        } else {
            pp = &cur->next;
        }
    }

    spinlock_release(&pmm_global_lock, flags);

    for (uint32_t j = 0; j < count; j++)
        page_zero((first + j) * PAGE_SIZE);

    return first * PAGE_SIZE;
}

/* NUMA-aware page allocation */
uint64_t pmm_alloc_node_pages(uint32_t count, int node) {
    if (count == 0) return 0;
    if (!numa_available) return pmm_alloc_pages(count);

    cpu_flags_t flags;
    spinlock_acquire(&pmm_global_lock, &flags);

    /* Flush all per-CPU caches first */
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_flags_t lflags;
        spinlock_acquire(&cpu_cache_lock[cpu], &lflags);
        int n = cpu_cache_count[cpu];
        for (int i = 0; i < n; i++) {
            uint64_t fphys = cpu_cache[cpu][i];
            uint64_t fidx = fphys / PAGE_SIZE;
            if (fidx < total_page_count) {
                bitmap_clear(fidx);
                owner_set(fidx, PAGE_OWNER_UNSET);
                global_free_count++;
                free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(fphys);
                fp->next = free_list;
                free_list = fp;
            }
        }
        cpu_cache_count[cpu] = 0;
        spinlock_release(&cpu_cache_lock[cpu], lflags);
    }

    /* Pass 1: try to find pages in the preferred node */
    uint64_t first = 0;
    uint32_t found = 0;
    for (uint64_t i = 0; i < total_page_count && found < count; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) {
                if (acpi_is_page_in_node(i, node)) {
                    first = i;
                    found++;
                }
            } else {
                found++;
            }
        } else {
            found = 0;
        }
    }

    /* Pass 2: fallback — any page */
    if (found < count) {
        first = 0;
        found = 0;
        for (uint64_t i = 0; i < total_page_count && found < count; i++) {
            if (!bitmap_test(i)) {
                if (found == 0) first = i;
                found++;
            } else {
                found = 0;
            }
        }
    }

    if (found < count) { spinlock_release(&pmm_global_lock, flags); return 0; }

    for (uint32_t j = 0; j < count; j++) {
        bitmap_set(first + j);
        global_free_count--;
        owner_set(first + j, (uint64_t)current_thread);
    }

    free_page_t** pp = &free_list;
    while (*pp) {
        free_page_t* cur = *pp;
        uint64_t pa = VIRT_TO_PHYS(cur);
        uint64_t pidx = pa / PAGE_SIZE;
        if (pidx >= first && pidx < first + count) {
            *pp = cur->next;
        } else {
            pp = &cur->next;
        }
    }

    spinlock_release(&pmm_global_lock, flags);

    for (uint32_t j = 0; j < count; j++)
        page_zero((first + j) * PAGE_SIZE);

    return first * PAGE_SIZE;
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0 || (phys_addr & 0xFFF)) return;

    /* Fast path: try per-CPU cache */
    if (cache_try_push(phys_addr))
        return;

    /* Cache full — flush half to global, then push */
    cpu_flags_t flags;
    spinlock_acquire(&pmm_global_lock, &flags);

    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_page_count) { spinlock_release(&pmm_global_lock, flags); return; }
    if (!bitmap_test(idx)) {
        kprintf("[PMM] Warning: double free detected: page %lx\n", phys_addr);
        spinlock_release(&pmm_global_lock, flags);
        return;
    }

    /* Flush half of our cache to global */
    cache_flush_half();

    /* Push the caller's page to the now-emptied cache, with atomic
     * page_owner update inside the per-CPU lock.  The page stays in the
     * per-CPU cache with bitmap SET — it is still "in-use" from the
     * bitmap's perspective.  DO NOT clear the bitmap here: if we cleared
     * it, the page would appear free to pmm_alloc_pages (which flushes
     * all caches + scans bitmap), causing a double-alloc. */
    int cpu = current_cpu();
    cpu_flags_t lflags;
    spinlock_acquire(&cpu_cache_lock[cpu], &lflags);
    int n = cpu_cache_count[cpu];

    /* Update page_owner inside the per-CPU lock so another CPU's
     * cache_try_pop sees CACHE immediately.  Use cmpxchg to prevent
     * TOCTOU with a concurrent cache_try_pop on another CPU. */
    if (page_owner && idx < total_page_count) {
        uint64_t old = page_owner[idx];
        if (old == PAGE_OWNER_CACHE) {
            kprintf("[PMM] CRASH: double-free page %lx (idx %lu) slow path!\n",
                    phys_addr, idx);
            spinlock_release(&cpu_cache_lock[cpu], lflags);
            spinlock_release(&pmm_global_lock, flags);
            for (;;) asm("cli; hlt");
        }
        if (!__sync_bool_compare_and_swap(&page_owner[idx], old, PAGE_OWNER_CACHE)) {
            /* Another CPU changed page_owner between our read and this
             * cmpxchg — the page was claimed concurrently.  Release locks
             * and bail; the caller's free is silently dropped (the page
             * is already in another cache or allocated). */
            spinlock_release(&cpu_cache_lock[cpu], lflags);
            spinlock_release(&pmm_global_lock, flags);
            return;
        }
        if (page_owner[idx] != PAGE_OWNER_CACHE) {
            kprintf("[PMM] CRASH: cmpxchg verify failed slow-free page %lx idx %lu\n",
                    phys_addr, idx);
            spinlock_release(&cpu_cache_lock[cpu], lflags);
            spinlock_release(&pmm_global_lock, flags);
            for (;;) asm("cli; hlt");
        }
    }

    cpu_cache[cpu][n] = phys_addr;
    cpu_cache_count[cpu] = n + 1;
    spinlock_release(&cpu_cache_lock[cpu], lflags);
    spinlock_release(&pmm_global_lock, flags);
}

uint64_t pmm_page_owner(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= total_page_count || !page_owner) return 0;
    return page_owner[idx];
}

void pmm_free_pages(uint64_t phys_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(phys_addr + i * PAGE_SIZE);
    }
}



/* ------------------------------------------------------------------ */
/*  Initialisation                                                    */
/* ------------------------------------------------------------------ */
static void add_region_to_free_list(uint64_t start, uint64_t end) {
    if (start >= end) return;
    uint64_t s = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t e = end & ~(PAGE_SIZE - 1);
    for (uint64_t phys = s; phys < e; phys += PAGE_SIZE) {
        uint64_t idx = phys / PAGE_SIZE;
        if (idx < total_page_count && !bitmap_test(idx)) {
            free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(phys);
            fp->next = free_list;
            free_list = fp;
        }
    }
}

static void parse_mb_mmap(uint64_t mb_info) {
    (void)mb_info;
    hal_mmap_entry_t entries[MAX_MMAP_ENTRIES];
    int n = hal_get_mmap_entries(entries, MAX_MMAP_ENTRIES);
    if (n == 0) {
        add_region_to_free_list(1 * 1024 * 1024, total_memory);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (entries[i].type == 1) {
            add_region_to_free_list(entries[i].start, entries[i].end);
        }
    }
}

err_t pmm_init(uint64_t mem_size_phys, uint64_t mb_info_phys) {
    spinlock_init(&pmm_global_lock, "pmm_global_lock");
    total_memory = mem_size_phys;
    total_page_count = mem_size_phys / PAGE_SIZE;

    /* Initialize per-CPU cache locks */
    for (int i = 0; i < MAX_CPUS; i++) {
        spinlock_init(&cpu_cache_lock[i], "pmm-cache");
        cpu_cache_count[i] = 0;
    }

    kprintf("[PMM] Total memory: %lu MB (%lu pages)\n",
            mem_size_phys / (1024 * 1024), total_page_count);

    uint64_t bitmap_size = (total_page_count + 7) / 8;
    bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_base = 0x10000;

    kmemset((void*)PHYS_TO_VIRT(bitmap_base), 0xFF, bitmap_pages * PAGE_SIZE);
    used_bitmap = (uint8_t*)(PHYS_TO_VIRT(bitmap_base));

    /* Allocate page ownership array right after the kernel image */
    extern uint64_t _kernel_end_phys;
    uint64_t owner_base = ((uint64_t)&_kernel_end_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t owner_bytes = total_page_count * sizeof(uint64_t);
    uint64_t owner_pages = (owner_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    kmemset((void*)PHYS_TO_VIRT(owner_base), 0, owner_pages * PAGE_SIZE);
    page_owner = (uint64_t*)(PHYS_TO_VIRT(owner_base));

    for (uint64_t i = 0; i < total_page_count; i++) {
        bitmap_clear(i);
    }
    global_free_count = total_page_count;

    pmm_mark_region_used(0, 0x1000);
    pmm_mark_region_used(0x4000, 0x7000);   /* SMP trampoline + data */
    pmm_mark_region_used(0x7000, 0xB000);
    pmm_mark_region_used(0xA0000, 0x100000);
    pmm_mark_region_used(0x10000, 0x10000 + bitmap_pages * PAGE_SIZE);
    pmm_mark_region_used(mem_size_phys - 0x1000, mem_size_phys);

    pmm_mark_region_used(0x100000, (uint64_t)&_kernel_end_phys);
    /* Mark page_owner array used — must come AFTER the bitmap_clear loop
     * above, which resets every bit. */
    pmm_mark_region_used(owner_base, owner_base + owner_pages * PAGE_SIZE);

    parse_mb_mmap(mb_info_phys);

    kprintf("[PMM] Free: %lu pages (%lu MB)\n",
            global_free_count, global_free_count * 4 / 1024);
    return ERR_OK;
}
