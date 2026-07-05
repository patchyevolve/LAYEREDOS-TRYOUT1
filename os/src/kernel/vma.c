#include "vma.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "hal.h"
#include "kernel.h"

/* Forward declaration of PAGE_DIRTY (x86-64 PTE bit 6) */
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_ACCESSED   (1ULL << 5)

/* Add a VMA to the process's linked list. */
vma_t* vma_add(process_t* proc, uint64_t start, uint64_t end,
               int prot, int flags, vfs_node_t* node, uint64_t file_offset) {
    vma_t* v = (vma_t*)kmalloc(sizeof(vma_t));
    if (!v) return NULL;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->file_offset = file_offset;
    v->next = NULL;

    if (node) {
        __sync_fetch_and_add(&node->refcount, 1);
    }
    v->node = node;

    cpu_flags_t _vf;
    spinlock_acquire(&proc->vma_lock, &_vf);
    v->next = (vma_t*)proc->vmas;
    proc->vmas = v;
    spinlock_release(&proc->vma_lock, _vf);
    return v;
}

/* Find VMA containing address. */
vma_t* vma_find(process_t* proc, uint64_t addr) {
    cpu_flags_t _vf;
    spinlock_acquire(&proc->vma_lock, &_vf);
    vma_t* v = (vma_t*)proc->vmas;
    while (v) {
        if (addr >= v->start && addr < v->end) {
            spinlock_release(&proc->vma_lock, _vf);
            return v;
        }
        v = v->next;
    }
    spinlock_release(&proc->vma_lock, _vf);
    return NULL;
}

/* Remove a VMA entry by unlinking and freeing the struct.
 * Does NOT unmap pages or free vfs_node — caller handles that. */
static void vma_free_entry(process_t* proc, vma_t* target) {
    vma_t** pp = (vma_t**)&proc->vmas;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            if (target->node)
                __sync_fetch_and_sub(&target->node->refcount, 1);
            kfree(target);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void vma_unmap_range(process_t* proc, uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        page_entry_t* pte = vmm_walk_pagetable(proc->cr3, p);
        if (!pte || !(*pte & PAGE_PRESENT)) continue;
        uint64_t phys = *pte & ~0xFFFULL;
        pmm_free_page(phys);
        vmm_unmap_page(proc->cr3, p);
    }
}

static void vma_unmap_range_shared(process_t* proc, uint64_t start, uint64_t end,
                                   vfs_node_t* node, uint64_t base_file_off) {
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        page_entry_t* pte = vmm_walk_pagetable(proc->cr3, p);
        if (!pte || !(*pte & PAGE_PRESENT)) continue;
        uint64_t phys = *pte & ~0xFFFULL;

        /* Write back dirty pages for MAP_SHARED */
        if ((*pte & PAGE_DIRTY) && node) {
            uint64_t file_off = base_file_off + (p - start);
            node->fs->ops->write(node, (const void*)PHYS_TO_VIRT(phys),
                                 PAGE_SIZE, file_off);
        }

        pmm_free_page(phys);
        vmm_unmap_page(proc->cr3, p);
    }
}

/* Remove VMA entries covering [addr, addr+len).  Unmaps pages in the
 * intersection; for MAP_SHARED dirty pages are written back to the file. */
err_t vma_remove(process_t* proc, uint64_t addr, size_t len) {
    if (addr & 0xFFF) return ERR_INVAL;
    size_t page_len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = addr + page_len;

    cpu_flags_t _vf;
    spinlock_acquire(&proc->vma_lock, &_vf);

    /* Collect all VMAs that intersect [addr, end) */
    vma_t* v = (vma_t*)proc->vmas;
    while (v) {
        vma_t* next = v->next;

        uint64_t inter_start = (addr > v->start) ? addr : v->start;
        uint64_t inter_end   = (end < v->end) ? end : v->end;

        if (inter_start < inter_end) {
            /* There is overlap */
            if (v->flags & 0x01) {
                /* MAP_SHARED: write-back dirty pages */
                uint64_t base_file_off = v->file_offset + (inter_start - v->start);
                vma_unmap_range_shared(proc, inter_start, inter_end, v->node,
                                       base_file_off);
            } else {
                vma_unmap_range(proc, inter_start, inter_end);
            }

            if (inter_start == v->start && inter_end == v->end) {
                /* VMA fully covered — remove entirely */
                vma_free_entry(proc, v);
            } else if (inter_start == v->start) {
                /* Overlap at start — truncate start upward */
                v->start = inter_end;
            } else if (inter_end == v->end) {
                /* Overlap at end — truncate end downward */
                v->end = inter_start;
            } else {
                /* Overlap in middle — split: create new VMA for the tail */
                vma_t* tail = (vma_t*)kmalloc(sizeof(vma_t));
                if (tail) {
                    tail->start = inter_end;
                    tail->end = v->end;
                    tail->prot = v->prot;
                    tail->flags = v->flags;
                    tail->file_offset = v->file_offset + (inter_end - v->start);
                    tail->next = v->next;
                    if (v->node) {
                        __sync_fetch_and_add(&v->node->refcount, 1);
                    }
                    tail->node = v->node;
                    v->next = tail;
                }
                v->end = inter_start;
            }
        }

        v = next;
    }
    spinlock_release(&proc->vma_lock, _vf);
    return ERR_OK;
}

/* Page fault handler for file-backed VMAs.
 * Called from the interrupt handler (vector 14) for user-mode faults
 * that are not swap pages.  Returns 1 if the fault was resolved,
 * 0 if the fault address does not belong to a file-backed VMA. */
int vma_handle_fault(process_t* proc, uint64_t fault_addr, uint64_t error_code) {
    (void)error_code;

    cpu_flags_t _vf;
    spinlock_acquire(&proc->vma_lock, &_vf);

    /* Inline search (lock already held) */
    vma_t* v = (vma_t*)proc->vmas;
    while (v) {
        if (fault_addr >= v->start && fault_addr < v->end)
            break;
        v = v->next;
    }
    if (!v) {
        spinlock_release(&proc->vma_lock, _vf);
        return 0;
    }

    uint64_t page_start = fault_addr & ~0xFFFULL;

    /* Already mapped?  This can happen for COW protection faults on
     * MAP_PRIVATE writable pages (mapped read-only, write triggers fault). */
    page_entry_t* pte = vmm_walk_pagetable(proc->cr3, page_start);
    if (pte && (*pte & PAGE_PRESENT)) {
        /* Page exists but protection fault (e.g., write to read-only).
         * For MAP_PRIVATE with PROT_WRITE, re-map writable. */
        if ((v->flags & 0x01) == 0 && (v->prot & 0x2)) {
            uint64_t phys = *pte & ~0xFFFULL;
            *pte = phys | PAGE_USER | PAGE_PRESENT | PAGE_WRITE;
            vmm_flush_tlb_page(page_start);
            spinlock_release(&proc->vma_lock, _vf);
            return 1;
        }
        spinlock_release(&proc->vma_lock, _vf);
        return 0;
    }

    /* Allocate a physical page */
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        spinlock_release(&proc->vma_lock, _vf);
        return 0;
    }
    kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);

    /* If file-backed, read the page content from the file */
    if (v->node) {
        uint64_t page_off = page_start - v->start;
        uint64_t file_off = v->file_offset + page_off;
        v->node->fs->ops->read(v->node, (void*)PHYS_TO_VIRT(phys),
                               PAGE_SIZE, file_off);
    }

    /* Determine mapping flags */
    uint64_t pgflags = PAGE_USER;
    if (v->prot & 0x2) {
        if (v->flags & 0x01) {
            /* MAP_SHARED with PROT_WRITE — map writable */
            pgflags |= PAGE_WRITE;
        } else {
            /* MAP_PRIVATE with PROT_WRITE — map writable (no COW) */
            pgflags |= PAGE_WRITE;
        }
    }

    vmm_map_page(proc->cr3, page_start, phys, pgflags);
    spinlock_release(&proc->vma_lock, _vf);
    return 1;
}

/* Duplicate VMA list for fork.  Shallow-copies vma_t structs,
 * increments vfs_node refcount for file-backed mappings. */
err_t vma_duplicate(process_t* dst) {
    process_t* src = NULL;
    if (current_thread) src = current_thread->proc;
    if (!src) return ERR_INVAL;

    cpu_flags_t _vf;
    spinlock_acquire(&src->vma_lock, &_vf);

    /* Build a reversed copy of the list to preserve order */
    vma_t* out = NULL;
    vma_t* v = (vma_t*)src->vmas;
    while (v) {
        vma_t* n = (vma_t*)kmalloc(sizeof(vma_t));
        if (!n) {
            spinlock_release(&src->vma_lock, _vf);
            vma_cleanup(dst);
            return ERR_NOMEM;
        }
        n->start = v->start;
        n->end = v->end;
        n->prot = v->prot;
        n->flags = v->flags;
        n->file_offset = v->file_offset;
        n->node = v->node;
        if (n->node)
            __sync_fetch_and_add(&n->node->refcount, 1);
        n->next = out;
        out = n;
        v = v->next;
    }
    spinlock_release(&src->vma_lock, _vf);
    dst->vmas = (void*)out;
    return ERR_OK;
}

/* Free all VMAs, release vfs_node references. */
void vma_cleanup(process_t* proc) {
    cpu_flags_t _vf;
    spinlock_acquire(&proc->vma_lock, &_vf);
    vma_t* v = (vma_t*)proc->vmas;
    while (v) {
        vma_t* next = v->next;
        if (v->node)
            __sync_fetch_and_sub(&v->node->refcount, 1);
        kfree(v);
        v = next;
    }
    proc->vmas = NULL;
    spinlock_release(&proc->vma_lock, _vf);
}
