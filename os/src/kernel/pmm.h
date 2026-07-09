#ifndef PMM_H
#define PMM_H

#include "kernel.h"

err_t pmm_init(uint64_t mem_size_phys, uint64_t mb_info_phys);
uint64_t pmm_alloc_page(void);
void  pmm_free_page(uint64_t phys_addr);
uint64_t pmm_alloc_pages(uint32_t count);
uint64_t pmm_alloc_node_pages(uint32_t count, int node);
void  pmm_free_pages(uint64_t phys_addr, uint32_t count);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
uint64_t pmm_used_pages(void);
void  pmm_mark_region_used(uint64_t start, uint64_t end);
void  pmm_flush_cpu_cache(int cpu);
uint64_t pmm_page_owner(uint64_t phys);

/* Zero a 4KB page using the kernel identity map. */
static inline void page_zero(uint64_t phys) {
    volatile uint64_t* p = (volatile uint64_t*)PHYS_TO_VIRT(phys);
    for (int i = 0; i < 512; i++)
        p[i] = 0;
}

/* Rebuild per-node free lists after SRAT parsing.
 * Called from acpi_parse_srat() when NUMA topology is discovered. */
void pmm_numa_init(void);

/* Get the NUMA node for the current CPU (0 if NUMA not available). */
int pmm_current_node(void);

#endif
