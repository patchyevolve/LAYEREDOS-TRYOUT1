#include "kernel.h"
#include "kmalloc.h"
#include "pmm.h"
#include "vmm.h"
#include "sync.h"

#ifdef CONFIG_SMP
#include "smp.h"
#endif

static spinlock_t kmalloc_lock;

#define SLAB_MAGIC 0x5B1AB5
#define SLAB_MAX_OBJS 128
#define MIN_SLAB_SHIFT 4
#define MAX_SLAB_SHIFT 10
#define MIN_SLAB_SIZE (1 << MIN_SLAB_SHIFT)
#define MAX_SLAB_SIZE (1 << MAX_SLAB_SHIFT)
#define SLAB_COUNT (MAX_SLAB_SHIFT - MIN_SLAB_SHIFT + 1)
#define SLAB_BITMAP_BYTES ((SLAB_MAX_OBJS + 7) / 8)
#define SLAB_HEADER_SIZE (sizeof(slab_page_t) + SLAB_BITMAP_BYTES)

typedef struct slab_page {
    struct slab_page* next;
    uint32_t magic;
    uint16_t free_count;
    uint16_t total;
    uint8_t  shift;
} slab_page_t;

static slab_page_t* slabs[SLAB_COUNT];
static size_t kmalloc_bytes_used = 0;
static size_t kmalloc_bytes_total = 0;

#ifdef CONFIG_SMP
#define KMAG_SIZE 8
static void* kmag_ptrs[MAX_CPUS][SLAB_COUNT][KMAG_SIZE];
static int   kmag_count[MAX_CPUS][SLAB_COUNT];
static spinlock_t kmag_lock[MAX_CPUS];

/* Flush all objects from cpu's magazine[idx] to slab bitmaps.
 * Caller must hold kmalloc_lock. */
static void kmag_flush(int cpu, int idx) {
    cpu_flags_t mag_flags;
    spinlock_acquire(&kmag_lock[cpu], &mag_flags);
    int n = kmag_count[cpu][idx];
    for (int i = 0; i < n; i++) {
        void* ptr = kmag_ptrs[cpu][idx][i];
        slab_page_t* pg = (slab_page_t*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
        if (pg->magic != SLAB_MAGIC) continue;
        uint16_t objsize = 1 << pg->shift;
        uint8_t* bmp = ((uint8_t*)pg) + sizeof(slab_page_t);
        uint8_t* base = (uint8_t*)pg + SLAB_HEADER_SIZE;
        int slot = (int)(((uint8_t*)ptr - base) / objsize);
        if (slot >= 0 && slot < pg->total) {
            bmp[slot / 8] |= (1 << (slot % 8));
            pg->free_count++;
            kmalloc_bytes_used -= objsize;
        }
    }
    kmag_count[cpu][idx] = 0;
    spinlock_release(&kmag_lock[cpu], mag_flags);
}

/* Flush the oldest half of objects from cpu's magazine[idx] to slab.
 * Caller must hold kmalloc_lock. */
static void kmag_flush_half(int cpu, int idx) {
    cpu_flags_t mag_flags;
    spinlock_acquire(&kmag_lock[cpu], &mag_flags);
    int n = kmag_count[cpu][idx];
    int keep = n / 2;
    for (int i = keep; i < n; i++) {
        void* ptr = kmag_ptrs[cpu][idx][i];
        slab_page_t* pg = (slab_page_t*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
        if (pg->magic != SLAB_MAGIC) continue;
        uint16_t objsize = 1 << pg->shift;
        uint8_t* bmp = ((uint8_t*)pg) + sizeof(slab_page_t);
        uint8_t* base = (uint8_t*)pg + SLAB_HEADER_SIZE;
        int slot = (int)(((uint8_t*)ptr - base) / objsize);
        if (slot >= 0 && slot < pg->total) {
            bmp[slot / 8] |= (1 << (slot % 8));
            pg->free_count++;
            kmalloc_bytes_used -= objsize;
        }
    }
    kmag_count[cpu][idx] = keep;
    spinlock_release(&kmag_lock[cpu], mag_flags);
}

/* Flush one CPU's magazines for all slab classes.
 * Used during CPU hotplug offlining. */
void kmag_flush_one_cpu(int cpu) {
    if (!smp_enabled || cpu < 0 || cpu >= MAX_CPUS) return;
    cpu_flags_t _sflags;
    spinlock_acquire(&kmalloc_lock, &_sflags);
    for (int i = 0; i < SLAB_COUNT; i++) {
        if (kmag_count[cpu][i] > 0)
            kmag_flush(cpu, i);
    }
    spinlock_release(&kmalloc_lock, _sflags);
}

/* Flush all CPUs' magazines for all slab classes. Used before compaction.
 * Caller must hold kmalloc_lock. */
static void kmag_flush_all(void) {
    for (int c = 0; c < MAX_CPUS; c++) {
        for (int i = 0; i < SLAB_COUNT; i++) {
            if (kmag_count[c][i] > 0)
                kmag_flush(c, i);
        }
    }
}
#endif

static int slab_index(size_t size) {
    if (size < MIN_SLAB_SIZE) size = MIN_SLAB_SIZE;
    int idx = 0;
    while ((1UL << (idx + MIN_SLAB_SHIFT)) < size) idx++;
    if (idx >= SLAB_COUNT) return -1;
    return idx;
}

void kmalloc_init(void) {
    kmemset(slabs, 0, sizeof(slabs));
    spinlock_init(&kmalloc_lock, "kmalloc_lock");
#ifdef CONFIG_SMP
    for (int i = 0; i < MAX_CPUS; i++) {
        kmemset(kmag_ptrs[i], 0, sizeof(kmag_ptrs[i]));
        kmemset(kmag_count[i], 0, sizeof(kmag_count[i]));
        spinlock_init(&kmag_lock[i], "kmag");
    }
#endif
}

static slab_page_t* slab_new_page(int idx) {
    uint64_t phys = pmm_alloc_node_pages(1, pmm_current_node());
    if (!phys) return NULL;
    uint64_t virt = PHYS_TO_VIRT(phys);
    kmemset((void*)virt, 0, PAGE_SIZE);
    uint16_t objsize = 1 << (idx + MIN_SLAB_SHIFT);
    slab_page_t* page = (slab_page_t*)virt;
    page->magic = SLAB_MAGIC;
    page->shift = idx + MIN_SLAB_SHIFT;
    page->total = (PAGE_SIZE - SLAB_HEADER_SIZE) / objsize;
    if (page->total > SLAB_MAX_OBJS) page->total = SLAB_MAX_OBJS;
    if (page->total < 1) page->total = 1;
    page->free_count = page->total;
    page->next = slabs[idx];
    slabs[idx] = page;
    
    // Initialize bitmap: set all bits to 1 (free)
    uint8_t* bitmap = ((uint8_t*)page) + sizeof(slab_page_t);
    kmemset(bitmap, 0xFF, SLAB_BITMAP_BYTES);
    // Mark slots beyond page->total as used
    for (int i = page->total; i < SLAB_MAX_OBJS; i++)
        bitmap[i/8] &= ~(1 << (i % 8));
    
    kmalloc_bytes_total += PAGE_SIZE;
    return page;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    int idx = slab_index(size);

#ifdef CONFIG_SMP
    /* Fast path: try per-CPU magazine (slab-sized only) */
    if (idx >= 0) {
        int cpu = smp_cpu_id();
        cpu_flags_t mag_flags;
        spinlock_acquire(&kmag_lock[cpu], &mag_flags);
        if (kmag_count[cpu][idx] > 0) {
            kmag_count[cpu][idx]--;
            void* ptr = kmag_ptrs[cpu][idx][kmag_count[cpu][idx]];
            spinlock_release(&kmag_lock[cpu], mag_flags);
            return ptr;
        }
        spinlock_release(&kmag_lock[cpu], mag_flags);
    }
#endif

    cpu_flags_t _sflags; spinlock_acquire(&kmalloc_lock, &_sflags);
    if (idx < 0) {
        uint64_t pages = (size + sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (!phys) { spinlock_release(&kmalloc_lock, _sflags); return NULL; }
        kmalloc_bytes_used += pages * PAGE_SIZE;
        kmalloc_bytes_total += pages * PAGE_SIZE;
        void* ptr = (void*)PHYS_TO_VIRT(phys);
        *(uint64_t*)ptr = pages;
        spinlock_release(&kmalloc_lock, _sflags);
        return (void*)((uint64_t)ptr + sizeof(uint64_t));
    }
    uint16_t objsize = 1 << (idx + MIN_SLAB_SHIFT);
    slab_page_t* page = slabs[idx];
    while (page) {
        if (page->magic != SLAB_MAGIC) { page = page->next; continue; }
        uint8_t* bitmap = ((uint8_t*)page) + sizeof(slab_page_t);
        for (int i = 0; i < page->total; i++) {
            if (bitmap[i / 8] & (1 << (i % 8))) {
                bitmap[i / 8] &= ~(1 << (i % 8));
                page->free_count--;
                kmalloc_bytes_used += objsize;
                uint8_t* base = (uint8_t*)page + SLAB_HEADER_SIZE;
                spinlock_release(&kmalloc_lock, _sflags);
                return base + (i * objsize);
            }
        }
        page = page->next;
    }
    slab_page_t* np = slab_new_page(idx);
    if (!np) { spinlock_release(&kmalloc_lock, _sflags); return NULL; }
    uint8_t* bitmap = ((uint8_t*)np) + sizeof(slab_page_t);
    bitmap[0] &= ~1;
    np->free_count--;
    kmalloc_bytes_used += objsize;
    uint8_t* base = (uint8_t*)np + SLAB_HEADER_SIZE;
    spinlock_release(&kmalloc_lock, _sflags);
    return base;
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t addr = (uint64_t)ptr;
    if (addr < KERNEL_VMA) return;
    uint64_t page_start = addr & ~(PAGE_SIZE - 1);
    slab_page_t* page = (slab_page_t*)page_start;

    if (page->magic != SLAB_MAGIC) {
        cpu_flags_t _sflags; spinlock_acquire(&kmalloc_lock, &_sflags);
        uint64_t alloc_start = page_start;
        uint64_t pages = *(uint64_t*)alloc_start;
        if (pages == 0 || pages > (1ULL << 20)) { spinlock_release(&kmalloc_lock, _sflags); return; }
        uint64_t phys = VIRT_TO_PHYS(alloc_start);
        pmm_free_pages(phys, pages);
        kmalloc_bytes_used -= pages * PAGE_SIZE;
        kmalloc_bytes_total -= pages * PAGE_SIZE;
        spinlock_release(&kmalloc_lock, _sflags);
        return;
    }

#ifdef CONFIG_SMP
    /* Fast path: try per-CPU magazine */
    {
        int s_idx = page->shift - MIN_SLAB_SHIFT;
        if (s_idx >= 0 && s_idx < SLAB_COUNT) {
            int cpu = smp_cpu_id();
            cpu_flags_t mag_flags;
            spinlock_acquire(&kmag_lock[cpu], &mag_flags);
            if (kmag_count[cpu][s_idx] < KMAG_SIZE) {
                kmag_ptrs[cpu][s_idx][kmag_count[cpu][s_idx]] = ptr;
                kmag_count[cpu][s_idx]++;
                spinlock_release(&kmag_lock[cpu], mag_flags);
                return;
            }
            spinlock_release(&kmag_lock[cpu], mag_flags);
            /* Magazine full — slow path below.
             * Re-read cpu after acquiring kmalloc_lock: we may have been
             * migrated between the mag_lock release and kmalloc_lock acquire.
             * Holding kmalloc_lock disables IRQs so our CPU is stable. */
            cpu_flags_t _sflags; spinlock_acquire(&kmalloc_lock, &_sflags);
            cpu = smp_cpu_id();
            kmag_flush_half(cpu, s_idx);
            {
                cpu_flags_t mf;
                spinlock_acquire(&kmag_lock[cpu], &mf);
                kmag_ptrs[cpu][s_idx][kmag_count[cpu][s_idx]] = ptr;
                kmag_count[cpu][s_idx]++;
                spinlock_release(&kmag_lock[cpu], mf);
            }
            spinlock_release(&kmalloc_lock, _sflags);
            return;
        }
    }
    /* idx out of range (shouldn't happen) — fall through to bitmap-based free */
#endif

    cpu_flags_t _sflags; spinlock_acquire(&kmalloc_lock, &_sflags);
    uint16_t objsize = 1 << page->shift;
    uint8_t* bitmap = ((uint8_t*)page) + sizeof(slab_page_t);
    uint8_t* base = (uint8_t*)page + SLAB_HEADER_SIZE;
    int slot_idx = ((uint8_t*)ptr - base) / objsize;
    if (slot_idx < 0 || slot_idx >= page->total) { spinlock_release(&kmalloc_lock, _sflags); return; }
    if (bitmap[slot_idx / 8] & (1 << (slot_idx % 8))) kpanic("kfree: double free detected at %p (slab %p, idx %d)", ptr, page, slot_idx);
    bitmap[slot_idx / 8] |= (1 << (slot_idx % 8));
    page->free_count++;
    kmalloc_bytes_used -= objsize;
    spinlock_release(&kmalloc_lock, _sflags);
}

/* Compact heap: free any completely empty slab pages back to PMM.
 * Returns the number of pages freed.  Called before OOM kill. */
size_t kmalloc_compact(void) {
    cpu_flags_t _sflags; spinlock_acquire(&kmalloc_lock, &_sflags);

#ifdef CONFIG_SMP
    /* Flush all per-CPU magazines first so bitmap free_counts are accurate */
    kmag_flush_all();
#endif

    size_t freed = 0;
    for (int i = 0; i < SLAB_COUNT; i++) {
        slab_page_t** pp = &slabs[i];
        while (*pp) {
            slab_page_t* pg = *pp;
            if (pg->magic != SLAB_MAGIC) { pp = &pg->next; continue; }
            if (pg->free_count == pg->total && pg->total > 0) {
                /* Entire slab page is free — return it to PMM */
                *pp = pg->next;
                uint64_t phys = VIRT_TO_PHYS((uint64_t)pg);
                pmm_free_pages(phys, 1);
                kmalloc_bytes_total -= PAGE_SIZE;
                freed++;
            } else {
                pp = &pg->next;
            }
        }
    }
    spinlock_release(&kmalloc_lock, _sflags);
    if (freed) kprintf("[KMALLOC] Compacted %zu pages\n", freed);
    return freed;
}

size_t kmalloc_used(void) { return kmalloc_bytes_used; }
size_t kmalloc_total(void) { return kmalloc_bytes_total; }
