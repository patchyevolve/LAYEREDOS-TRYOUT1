#ifndef PMM_H
#define PMM_H

#include "types.h"

#define PAGE_SIZE 4096

err_t pmm_init(uint64_t mem_size_phys, uint64_t mb_info_phys);
uint64_t pmm_alloc_page(void);
void  pmm_free_page(uint64_t phys_addr);
uint64_t pmm_alloc_pages(uint32_t count);
void  pmm_free_pages(uint64_t phys_addr, uint32_t count);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
uint64_t pmm_used_pages(void);
void  pmm_mark_region_used(uint64_t start, uint64_t end);
void  pmm_debug_dump(void);

#endif
