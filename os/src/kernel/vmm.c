#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "hal.h"
#include "smp.h"
#include "acpi.h"


static uint64_t kernel_pml4 = 0;

uint64_t vmm_alloc_page_table(void) {
    uint64_t phys = pmm_alloc_node_pages(1, pmm_current_node());
    if (phys) kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    return phys;
}

static page_entry_t* get_entry(uint64_t pml4_phys, uint64_t virt,
                                int level, int create, uint64_t flags) {
    uint64_t table = pml4_phys;
    int indices[] = { PML4_INDEX(virt), PDPT_INDEX(virt),
                      PD_INDEX(virt), PT_INDEX(virt) };

    for (int l = 0; l < level; l++) {
        page_entry_t* entries = (page_entry_t*)PHYS_TO_VIRT(table);
        page_entry_t entry = entries[indices[l]];

        if (l == level - 1) return &entries[indices[l]];

        if (!(entry & PAGE_PRESENT)) {
            if (!create) return NULL;
            uint64_t new_table = vmm_alloc_page_table();
            if (!new_table) return NULL;
            entry = new_table | PAGE_PRESENT | PAGE_WRITE;
            if (flags & PAGE_USER) {
                entry |= PAGE_USER;
            }
            entries[indices[l]] = entry;
        }

        table = entry & ~0xFFFULL;
    }

    return NULL;
}

uint64_t vmm_get_kernel_pml4(void) { return kernel_pml4; }

page_entry_t* vmm_walk_pagetable(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entries;
    uint64_t table = pml4_phys;

    uint64_t pml4_idx = PML4_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pml4_idx] & PAGE_PRESENT)) return NULL;
    table = entries[pml4_idx] & ~0xFFFULL;

    uint64_t pdpt_idx = PDPT_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pdpt_idx] & PAGE_PRESENT)) return NULL;
    if (entries[pdpt_idx] & PAGE_HUGE) return &entries[pdpt_idx];
    table = entries[pdpt_idx] & ~0xFFFULL;

    uint64_t pd_idx = PD_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pd_idx] & PAGE_PRESENT)) return NULL;
    if (entries[pd_idx] & PAGE_HUGE) return &entries[pd_idx];
    table = entries[pd_idx] & ~0xFFFULL;

    uint64_t pt_idx = PT_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pt_idx] & PAGE_PRESENT)) return NULL;
    return &entries[pt_idx];
}

static page_entry_t* walk_intermediates(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entries;
    uint64_t table = pml4_phys;

    uint64_t pml4_idx = PML4_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pml4_idx] & PAGE_PRESENT)) return NULL;
    table = entries[pml4_idx] & ~0xFFFULL;

    uint64_t pdpt_idx = PDPT_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pdpt_idx] & PAGE_PRESENT)) return NULL;
    table = entries[pdpt_idx] & ~0xFFFULL;

    uint64_t pd_idx = PD_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    if (!(entries[pd_idx] & PAGE_PRESENT)) return NULL;
    table = entries[pd_idx] & ~0xFFFULL;

    uint64_t pt_idx = PT_INDEX(virt);
    entries = (page_entry_t*)PHYS_TO_VIRT(table);
    return &entries[pt_idx];
}

page_entry_t* vmm_peek_pte(uint64_t pml4_phys, uint64_t virt) {
    return walk_intermediates(pml4_phys, virt);
}

err_t vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                   uint64_t flags) {
    if (virt & 0xFFF) return ERR_INVAL;
    if (phys & 0xFFF) return ERR_INVAL;

    page_entry_t* pt = get_entry(pml4_phys, virt, 4, 1, flags);
    if (!pt) return ERR_NOMEM;

    *pt = (phys & ~0xFFFULL) | (flags & (0xFFF | PAGE_NX)) | PAGE_PRESENT;
    vmm_flush_tlb_page(virt);
    return ERR_OK;
}

err_t vmm_unmap_page(uint64_t pml4_phys, uint64_t virt) {
    page_entry_t* entry = vmm_walk_pagetable(pml4_phys, virt);
    if (!entry) return ERR_NOENT;

    *entry = 0;
    vmm_flush_tlb_page(virt);
    return ERR_OK;
}

void vmm_flush_tlb_page(uint64_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    /* On SMP, also trigger remote TLB flush */
    smp_tlb_shootdown_safe(virt, virt + PAGE_SIZE);
}

err_t vmm_duplicate_user_pages(uint64_t dst_pml4, uint64_t src_pml4) {
    cpu_flags_t flags = hal_save_irq();
    page_entry_t* src_entries = (page_entry_t*)PHYS_TO_VIRT(src_pml4);
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(src_entries[pml4_idx] & PAGE_PRESENT)) continue;
        uint64_t src_pdpt_phys = src_entries[pml4_idx] & ~0xFFFULL;
        /* Skip self-reference (PML4 entry pointing to PML4 itself)
         * and PID stamp at index 255 (bit 0 set makes it look present). */
        if (src_pdpt_phys == src_pml4) continue;
        if (pml4_idx == 255) continue;
        page_entry_t* src_pdpt = (page_entry_t*)PHYS_TO_VIRT(src_pdpt_phys);
        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(src_pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
            if (src_pdpt[pdpt_idx] & PAGE_HUGE) continue;
            uint64_t src_pd_phys = src_pdpt[pdpt_idx] & ~0xFFFULL;
            page_entry_t* src_pd = (page_entry_t*)PHYS_TO_VIRT(src_pd_phys);
            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(src_pd[pd_idx] & PAGE_PRESENT)) continue;
                if (src_pd[pd_idx] & PAGE_HUGE) continue;
                uint64_t src_pt_phys = src_pd[pd_idx] & ~0xFFFULL;
                page_entry_t* src_pt = (page_entry_t*)PHYS_TO_VIRT(src_pt_phys);
                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(src_pt[pt_idx] & PAGE_PRESENT)) continue;
                    uint64_t virt = ((uint64_t)pml4_idx << 39) |
                                    ((uint64_t)pdpt_idx << 30) |
                                    ((uint64_t)pd_idx << 21) |
                                    ((uint64_t)pt_idx << 12);
                    uint64_t src_phys = src_pt[pt_idx] & ~0xFFFULL;
                    uint64_t flags2 = src_pt[pt_idx] & 0xFFF;
                    uint64_t new_phys = pmm_alloc_node_pages(1, pmm_current_node());
                    if (!new_phys) { hal_restore_irq(flags); return ERR_NOMEM; }
                    kmemcpy((void*)PHYS_TO_VIRT(new_phys),
                            (void*)PHYS_TO_VIRT(src_phys), PAGE_SIZE);
                    vmm_map_page(dst_pml4, virt, new_phys, flags2);
                }
            }
        }
    }
    hal_restore_irq(flags);
    return ERR_OK;
}

void vmm_free_user_pages(uint64_t pml4_phys) {
    page_entry_t* pml4 = (page_entry_t*)PHYS_TO_VIRT(pml4_phys);
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(pml4[pml4_idx] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
        if (pdpt_phys == pml4_phys || pdpt_phys == 0) {
            pml4[pml4_idx] = 0;
            continue;
        }
        page_entry_t* pdpt = (page_entry_t*)PHYS_TO_VIRT(pdpt_phys);
        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
            if (pdpt[pdpt_idx] & PAGE_HUGE) {
                pmm_free_pages(pdpt[pdpt_idx] & ~0x3FFFFFFFULL, 262144);
                continue;
            }
            uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
            page_entry_t* pd = (page_entry_t*)PHYS_TO_VIRT(pd_phys);
            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PAGE_PRESENT)) continue;
                if (pd[pd_idx] & PAGE_HUGE) {
                    pmm_free_pages(pd[pd_idx] & ~0x1FFFFFULL, 512);
                    continue;
                }
                uint64_t pt_phys = pd[pd_idx] & ~0xFFFULL;
                page_entry_t* pt = (page_entry_t*)PHYS_TO_VIRT(pt_phys);
                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PAGE_PRESENT)) continue;
                    uint64_t page_phys = pt[pt_idx] & ~0xFFFULL;
                    pmm_free_page(page_phys);
                }
                pmm_free_page(pt_phys);
            }
            pmm_free_page(pd_phys);
        }
        pmm_free_page(pdpt_phys);
        pml4[pml4_idx] = 0;
    }
}

#ifdef VMM_DEBUG
void vmm_dump_pml4(uint64_t pml4_phys) {
    page_entry_t* pml4 = (page_entry_t*)PHYS_TO_VIRT(pml4_phys);
    kprintf("[VMM] PML4 dump at phys=0x%lx:\n", pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[i] & ~0xFFFULL;
        kprintf("  pml4[%u]=0x%lx -> pdpt=0x%lx\n", i, pml4[i], pdpt_phys);
        page_entry_t* pdpt = (page_entry_t*)PHYS_TO_VIRT(pdpt_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[j] & ~0xFFFULL;
            kprintf("    pdpt[%u]=0x%lx -> pd=0x%lx\n", j, pdpt[j], pd_phys);
        }
    }
}
#endif

#ifdef VMM_VALIDATE
err_t vmm_validate_pagetables(uint64_t pml4_phys) {
    page_entry_t* pml4 = (page_entry_t*)PHYS_TO_VIRT(pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[i] & ~0xFFFULL;
        if (pdpt_phys == pml4_phys) {
            kprintf("[VMM] VALIDATE: pml4[%d] self-references pml4=0x%lx\n", i, pml4_phys);
            return ERR_INVAL;
        }
        page_entry_t* pdpt = (page_entry_t*)PHYS_TO_VIRT(pdpt_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[j] & ~0xFFFULL;
            if (pd_phys == pml4_phys || pd_phys == pdpt_phys) {
                kprintf("[VMM] VALIDATE: pdpt[%d] points to pml4/pdpt (0x%lx)\n", j, pd_phys);
                return ERR_INVAL;
            }
            page_entry_t* pd = (page_entry_t*)PHYS_TO_VIRT(pd_phys);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT)) continue;
                uint64_t pt_phys = pd[k] & ~0xFFFULL;
                if (pt_phys == pml4_phys || pt_phys == pdpt_phys || pt_phys == pd_phys) {
                    kprintf("[VMM] VALIDATE: pd[%d] points to parent table (0x%lx)\n", k, pt_phys);
                    return ERR_INVAL;
                }
            }
        }
    }
    return ERR_OK;
}
#endif

err_t vmm_init(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = cr3;

    kprintf("[VMM] Layer 4 initialized: kernel PML4 at %lx\n", kernel_pml4);

    uint64_t kernel_end = (uint64_t)&_kernel_end - KERNEL_VMA_BASE;
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kprintf("[VMM] Kernel occupies 0x100000 - %lx\n", 0x100000 + kernel_end);

    return ERR_OK;
}

void vmm_split_identity_map(void) {
    /* Split 2MB superpages in the kernel identity map into 4KB page tables.
     * This works around a QEMU TCG softmmu TLB bug that silently drops stores
     * to byte 128 of any 2MB-mapped page under multi-vCPU contention.
     *
     * Must be called AFTER all ACPI table parsing (acpi_init, smp_init, apic_init)
     * but BEFORE any multi-vCPU activity, because:
     * 1. The page tables overwrite "available" physical pages that may still
     *    hold ACPI table data — call before ACPI init would corrupt the tables.
     * 2. Stores through 2MB superpages work reliably on single-CPU (the TCG bug
     *    only manifests with multiple vCPUs contending the TLB).
     * After the CR3 reload below, all identity-map accesses use 4KB PTEs. */
    page_entry_t* pml4 = (page_entry_t*)PHYS_TO_VIRT(kernel_pml4);
    if (!(pml4[511] & PAGE_PRESENT)) return;
    uint64_t pdpt_phys = pml4[511] & ~0xFFFULL;
    page_entry_t* pdpt = (page_entry_t*)PHYS_TO_VIRT(pdpt_phys);
    if (!(pdpt[511] & PAGE_PRESENT) || (pdpt[511] & PAGE_HUGE)) return;
    uint64_t pd_phys = pdpt[511] & ~0xFFFULL;
    page_entry_t* pd = (page_entry_t*)PHYS_TO_VIRT(pd_phys);
    int split_count = 0;
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        if (!(pd[i] & PAGE_HUGE)) continue;

        uint64_t base = pd[i] & ~0x1FFFFFULL;
        uint64_t pt_phys = pmm_alloc_node_pages(1, pmm_current_node());
        if (!pt_phys) {
            kprintf("[VMM] OOM splitting PD[%d], keeping 2MB page\n", i);
            continue;
        }
        page_entry_t* pt = (page_entry_t*)PHYS_TO_VIRT(pt_phys);
        for (int j = 0; j < 512; j++)
            pt[j] = (base + j * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;

        pd[i] = pt_phys | PAGE_PRESENT | PAGE_WRITE;
        split_count++;
    }
    if (split_count)
        kprintf("[VMM] Split %d 2MB superpages -> 4KB page tables\n", split_count);

    /* Full TLB flush: reload CR3 to discard any cached 2MB superpage entries */
    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");

    /* Re-apply kernel text/rodata read-only protection — the split created
     * writable PTEs; vmm_protect_kernel_text() was already called once before
     * the split on the original 2MB superpage entries, but the split replaced
     * those entries with 4KB page tables that have PAGE_WRITE set. */
    vmm_protect_kernel_text();
}

void vmm_protect_kernel_text(void) {
    uint64_t start = (uint64_t)&_text_start;
    uint64_t end   = (uint64_t)&_rodata_end;
    start &= ~0xFFFULL;
    end = (end + 0xFFF) & ~0xFFFULL;

    for (uint64_t addr = start; addr < end; addr += 0x1000) {
        page_entry_t* pte = vmm_walk_pagetable(kernel_pml4, addr);
        if (pte && (*pte & PAGE_PRESENT)) {
            *pte &= ~PAGE_WRITE;
            asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
        }
    }
    kprintf("[VMM] Kernel text+rodata read-only: %lx - %lx\n", start, end);
}
