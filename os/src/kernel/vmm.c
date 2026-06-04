#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "hal.h"

#define PML4_INDEX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FF)

static uint64_t kernel_pml4 = 0;

uint64_t vmm_alloc_page_table(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys) kmemset((void*)(uint64_t)phys, 0, PAGE_SIZE);
    return phys;
}

void vmm_free_page_table(uint64_t phys_addr) {
    if (phys_addr) pmm_free_page(phys_addr);
}

static page_entry_t* get_entry(uint64_t pml4_phys, uint64_t virt,
                               int level, int create) {
    uint64_t table = pml4_phys;
    int indices[] = { PML4_INDEX(virt), PDPT_INDEX(virt),
                      PD_INDEX(virt), PT_INDEX(virt) };

    for (int l = 0; l < level; l++) {
        page_entry_t* entries = (page_entry_t*)(uint64_t)table;
        page_entry_t entry = entries[indices[l]];

        if (l == level - 1) return &entries[indices[l]];

        if (!(entry & PAGE_PRESENT)) {
            if (!create) return NULL;
            uint64_t new_table = vmm_alloc_page_table();
            if (!new_table) return NULL;
            entry = new_table | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
            entries[indices[l]] = entry;
        }

        table = entry & ~0xFFFULL;
    }

    return NULL;
}

static page_entry_t* walk_pagetable(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entries;
    uint64_t table = pml4_phys;

    uint64_t pml4_idx = PML4_INDEX(virt);
    entries = (page_entry_t*)(uint64_t)table;
    if (!(entries[pml4_idx] & PAGE_PRESENT)) return NULL;
    table = entries[pml4_idx] & ~0xFFFULL;

    uint64_t pdpt_idx = PDPT_INDEX(virt);
    entries = (page_entry_t*)(uint64_t)table;
    if (!(entries[pdpt_idx] & PAGE_PRESENT)) return NULL;
    if (entries[pdpt_idx] & PAGE_HUGE) return &entries[pdpt_idx];
    table = entries[pdpt_idx] & ~0xFFFULL;

    uint64_t pd_idx = PD_INDEX(virt);
    entries = (page_entry_t*)(uint64_t)table;
    if (!(entries[pd_idx] & PAGE_PRESENT)) return NULL;
    if (entries[pd_idx] & PAGE_HUGE) return &entries[pd_idx];
    table = entries[pd_idx] & ~0xFFFULL;

    uint64_t pt_idx = PT_INDEX(virt);
    entries = (page_entry_t*)(uint64_t)table;
    if (!(entries[pt_idx] & PAGE_PRESENT)) return NULL;
    return &entries[pt_idx];
}

err_t vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                   uint64_t flags) {
    if (virt & 0xFFF) return ERR_INVAL;
    if (phys & 0xFFF) return ERR_INVAL;

    page_entry_t* pt = get_entry(pml4_phys, virt, 4, 1);
    if (!pt) return ERR_NOMEM;

    *pt = (phys & ~0xFFFULL) | (flags & 0xFFF) | PAGE_PRESENT;
    vmm_flush_tlb_page(virt);
    return ERR_OK;
}

err_t vmm_unmap_page(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entry = walk_pagetable(pml4_phys, virt);
    if (!entry) return ERR_NOENT;

    *entry = 0;
    vmm_flush_tlb_page(virt);
    return ERR_OK;
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entry = walk_pagetable(pml4_phys, virt);
    if (!entry) return 0;
    uint64_t phys = (*entry) & ~0xFFFULL;
    phys |= virt & 0xFFF;
    return phys;
}

err_t vmm_map_region(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                     uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        err_t e = vmm_map_page(pml4_phys, virt + i * PAGE_SIZE,
                               phys + i * PAGE_SIZE, flags);
        if (e) return e;
    }
    return ERR_OK;
}

err_t vmm_unmap_region(uint64_t pml4_phys, uint64_t virt, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        vmm_unmap_page(pml4_phys, virt + i * PAGE_SIZE);
    }
    return ERR_OK;
}

void vmm_flush_tlb(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3));
}

void vmm_flush_tlb_page(uint64_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_switch_pml4(uint64_t pml4_phys) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

err_t vmm_init(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = cr3;

    kprintf("[VMM] Layer 4 initialized: kernel PML4 at 0x%lx\n", kernel_pml4);

    uint64_t kernel_end = (uint64_t)&_kernel_end - KERNEL_VMA_BASE;
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kprintf("[VMM] Kernel occupies 0x100000 - 0x%lx\n", 0x100000 + kernel_end);

    return ERR_OK;
}
