#include "kernel.h"
#include "pmm.h"
#include "hal.h"
#include "eventbus.h"
#include "sched.h"
#include "process.h"
#include "work.h"
#include "kmalloc.h"

typedef struct free_page {
    struct free_page* next;
} free_page_t;

static free_page_t* free_list = NULL;
static uint64_t total_page_count = 0;
static uint64_t free_page_count = 0;
static uint64_t total_memory = 0;

static uint64_t bitmap_base = 0;
static uint64_t bitmap_pages = 0;
static uint8_t* used_bitmap; /* Uses PHYS_TO_VIRT address for per-PML4 safety */

static work_item_t oom_work_item;
static volatile int oom_scheduled = 0;

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
        /* Try heap compaction first — may free enough pages */
        kmalloc_compact();
        oom_scheduled = 1;
        oom_work_item.func = pmm_oom_kill_worker;
        oom_work_item.data = NULL;
        work_queue_schedule(&system_wq, &oom_work_item);
    }
}

static void bitmap_set(uint64_t page_idx) {
    used_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}
static void bitmap_clear(uint64_t page_idx) {
    used_bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
}
static int bitmap_test(uint64_t page_idx) {
    return (used_bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}

uint64_t pmm_total_pages(void) { return total_page_count; }
uint64_t pmm_free_pages_count(void) { return free_page_count; }
uint64_t pmm_used_pages(void) { return total_page_count - free_page_count; }

void pmm_mark_region_used(uint64_t start, uint64_t end) {
    uint64_t sp = start / PAGE_SIZE;
    uint64_t ep = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = sp; i < ep && i < total_page_count; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_page_count--;
        }
    }
}

uint64_t pmm_alloc_page(void) {
    cpu_flags_t flags = hal_save_irq();
    if (!free_list) { hal_restore_irq(flags); pmm_oom_kill(); return 0; }
    /* 0 is used as OOM sentinel; page 0 is reserved in pmm_init so this is unambiguous */

    free_page_t* page = free_list;
    uint64_t phys = VIRT_TO_PHYS(page);
    uint64_t idx = phys / PAGE_SIZE;

    /* Validate free-list pointer */
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
    free_page_count--;

    if (bitmap_test(idx)) {
        kprintf("[PMM] CRASH: page %lx (idx %lu) DOUBLE-ALLOCATED! free_list=%lx\n",
                phys, idx, (uint64_t)page->next);
        kprintf("[PMM] free_page_count=%lu total=%lu\n", free_page_count, total_page_count);
        for (;;) asm("cli; hlt");
    }
    bitmap_set(idx);
    hal_restore_irq(flags);
    kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    return phys;
}

uint64_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;

    cpu_flags_t flags = hal_save_irq();
    uint64_t first = 0;
    uint32_t found = 0;

    for (uint64_t i = 0; i < total_page_count && found < count; i++) {
        if ((i & 0xFFFF) == 0) { hal_restore_irq(flags); flags = hal_save_irq(); }
        if (!bitmap_test(i)) {
            if (found == 0) first = i;
            found++;
        } else {
            found = 0;
        }
    }

    if (found < count) { hal_restore_irq(flags); return 0; }

    for (uint32_t j = 0; j < count; j++) {
        bitmap_set(first + j);
        free_page_count--;
    }

    /* Single O(N) pass to remove allocated pages from free list */
    free_page_t** pp = &free_list;
    while (*pp) {
        free_page_t* cur = *pp;
        uint64_t pa = VIRT_TO_PHYS(cur);
        uint64_t idx = pa / PAGE_SIZE;
        if (idx >= first && idx < first + count) {
            *pp = cur->next;
        } else {
            pp = &cur->next;
        }
    }

    hal_restore_irq(flags);

    for (uint32_t j = 0; j < count; j++)
        kmemset((void*)PHYS_TO_VIRT((first + j) * PAGE_SIZE), 0, PAGE_SIZE);

    return first * PAGE_SIZE;
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0 || (phys_addr & 0xFFF)) return;
    cpu_flags_t flags = hal_save_irq();
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_page_count) { hal_restore_irq(flags); return; }
    if (!bitmap_test(idx)) {
#ifdef DEBUG
        kpanic("Double free detected: page %lx already free", phys_addr);
#else
        kprintf("[PMM] Warning: double free detected: page %lx\n", phys_addr);
        hal_restore_irq(flags);
        return;
#endif
    }

    bitmap_clear(idx);
    free_page_count++;

    free_page_t* page = (free_page_t*)PHYS_TO_VIRT(phys_addr);
    page->next = free_list;
    free_list = page;
    hal_restore_irq(flags);
}

void pmm_free_pages(uint64_t phys_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(phys_addr + i * PAGE_SIZE);
    }
}

static void add_region_to_free_list(uint64_t start, uint64_t end) {
    if (start >= end) return;
    uint64_t s = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t e = end & ~(PAGE_SIZE - 1);
    uint64_t count = 0;

    for (uint64_t phys = s; phys < e; phys += PAGE_SIZE) {
        uint64_t idx = phys / PAGE_SIZE;
        if (idx < total_page_count && !bitmap_test(idx)) {
            free_page_t* fp = (free_page_t*)PHYS_TO_VIRT(phys);
            fp->next = free_list;
            free_list = fp;
            count++;
        }
    }
    (void)count;
}

static void parse_mb_mmap(uint64_t mb_info) {
    (void)mb_info;
    hal_mmap_entry_t entries[MAX_MMAP_ENTRIES];
    int n = hal_get_mmap_entries(entries, MAX_MMAP_ENTRIES);
    if (n == 0) {
        /* No memory map, using contiguous range */
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
    total_memory = mem_size_phys;
    total_page_count = mem_size_phys / PAGE_SIZE;

    kprintf("[PMM] Total memory: %lu MB (%lu pages)\n",
            mem_size_phys / (1024 * 1024), total_page_count);

    uint64_t bitmap_size = (total_page_count + 7) / 8;
    bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_base = 0x10000;

    kmemset((void*)PHYS_TO_VIRT(bitmap_base), 0xFF, bitmap_pages * PAGE_SIZE);
    used_bitmap = (uint8_t*)(PHYS_TO_VIRT(bitmap_base));

    for (uint64_t i = 0; i < total_page_count; i++) {
        bitmap_clear(i);
    }
    free_page_count = total_page_count;

    pmm_mark_region_used(0, 0x1000);
    pmm_mark_region_used(0x7000, 0xB000);
    pmm_mark_region_used(0x10000, 0x10000 + bitmap_pages * PAGE_SIZE);
    pmm_mark_region_used(mem_size_phys - 0x1000, mem_size_phys);

    extern uint64_t _kernel_end_phys;
    pmm_mark_region_used(0x100000, (uint64_t)&_kernel_end_phys);

    parse_mb_mmap(mb_info_phys);

    kprintf("[PMM] Free: %lu pages (%lu MB)\n",
            free_page_count, free_page_count * 4 / 1024);
    return ERR_OK;
}
