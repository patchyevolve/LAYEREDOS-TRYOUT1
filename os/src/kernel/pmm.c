#include "kernel.h"
#include "pmm.h"
#include "hal.h"

typedef struct free_page {
    struct free_page* next;
} free_page_t;

static free_page_t* free_list = NULL;
static uint64_t total_page_count = 0;
static uint64_t free_page_count = 0;
static uint64_t total_memory = 0;

static uint64_t bitmap_base = 0;
static uint64_t bitmap_pages = 0;
static uint8_t* used_bitmap;

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
    if (!free_list) return 0;

    free_page_t* page = free_list;
    free_list = page->next;
    free_page_count--;

    uint64_t addr = (uint64_t)page;
    bitmap_set(addr / PAGE_SIZE);
    kmemset((void*)addr, 0, PAGE_SIZE);
    return addr;
}

uint64_t pmm_alloc_pages(uint32_t count) {
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

    if (found < count) return 0;

    for (uint32_t j = 0; j < count; j++) {
        uint64_t addr = (first + j) * PAGE_SIZE;
        bitmap_set(first + j);
        free_page_count--;

        free_page_t** pp = &free_list;
        while (*pp) {
            if ((uint64_t)*pp == addr) {
                *pp = (*pp)->next;
                break;
            }
            pp = &(*pp)->next;
        }

        kmemset((void*)addr, 0, PAGE_SIZE);
    }

    return first * PAGE_SIZE;
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0 || (phys_addr & 0xFFF)) return;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_page_count) return;
    if (!bitmap_test(idx)) return;

    bitmap_clear(idx);
    free_page_count++;

    free_page_t* page = (free_page_t*)phys_addr;
    page->next = free_list;
    free_list = page;
}

void pmm_free_pages(uint64_t phys_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(phys_addr + i * PAGE_SIZE);
    }
}

void pmm_debug_dump(void) {
    kprintf("[PMM] Total: %lu pages (%lu MB), Free: %lu pages (%lu MB)\n",
            total_page_count, total_page_count * 4 / 1024,
            free_page_count, free_page_count * 4 / 1024);
}

static void add_region_to_free_list(uint64_t start, uint64_t end) {
    uint64_t s = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t e = end & ~(PAGE_SIZE - 1);

    for (uint64_t addr = s; addr < e; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_page_count && !bitmap_test(idx)) {
            free_page_t* fp = (free_page_t*)addr;
            fp->next = free_list;
            free_list = fp;
        }
    }
}

static void parse_mb_mmap(uint64_t mb_info) {
    typedef struct {
        uint32_t size;
        uint64_t base_addr;
        uint64_t length;
        uint32_t type;
    } __attribute__((packed)) mmap_entry_t;

    typedef struct {
        uint32_t flags;
        uint32_t mem_lower;
        uint32_t mem_upper;
        uint32_t boot_device;
        uint32_t cmdline;
        uint32_t mods_count;
        uint32_t mods_addr;
        uint32_t syms[4];
        uint32_t mmap_length;
        uint32_t mmap_addr;
        uint32_t drives_length;
        uint32_t drives_addr;
    } __attribute__((packed)) multiboot_info_t;

    if (mb_info == 0) {
        kprintf("[PMM] No memory map (PVH boot)\n");
        add_region_to_free_list(1 * 1024 * 1024, total_memory);
        return;
    }

    multiboot_info_t* mbi = (multiboot_info_t*)(uint64_t)mb_info;

    if (!(mbi->flags & (1 << 6))) {
        kprintf("[PMM] No memory map from bootloader\n");
        add_region_to_free_list(1 * 1024 * 1024, total_memory);
        return;
    }

    mmap_entry_t* entry = (mmap_entry_t*)(uint64_t)mbi->mmap_addr;
    uint32_t remaining = mbi->mmap_length;

    while (remaining > 0) {
        uint32_t entry_size = entry->size + 4;
        uint64_t start = entry->base_addr;
        uint64_t end = entry->base_addr + entry->length;

        if (entry->type == 1) {
            add_region_to_free_list(start, end);
        }

        entry = (mmap_entry_t*)((uint64_t)entry + entry_size);
        if (entry_size < 4) break;
        remaining -= (remaining >= entry_size) ? entry_size : remaining;
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

    kmemset((void*)bitmap_base, 0xFF, bitmap_pages * PAGE_SIZE);
    used_bitmap = (uint8_t*)(uint64_t)bitmap_base;

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
