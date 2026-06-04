#ifndef VMM_H
#define VMM_H

#include "types.h"

#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_HUGE       (1ULL << 7)
#define PAGE_NX         (1ULL << 63)

typedef uint64_t page_entry_t;

err_t  vmm_init(void);
uint64_t vmm_alloc_page_table(void);
void  vmm_free_page_table(uint64_t phys_addr);
err_t vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                   uint64_t flags);
err_t vmm_unmap_page(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt);
err_t vmm_map_region(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                     uint64_t pages, uint64_t flags);
err_t vmm_unmap_region(uint64_t pml4_phys, uint64_t virt, uint64_t pages);
void  vmm_flush_tlb(void);
void  vmm_flush_tlb_page(uint64_t virt);
void  vmm_switch_pml4(uint64_t pml4_phys);

#endif
