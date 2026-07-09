#ifndef VMA_H
#define VMA_H

#include "types.h"
#include "vfs.h"

typedef struct process_t process_t;

typedef struct vma {
    uint64_t start;
    uint64_t end;
    int      prot;
    int      flags;
    vfs_node_t* node;
    uint64_t file_offset;
    struct vma* next;
} vma_t;

vma_t* vma_add(process_t* proc, uint64_t start, uint64_t end,
               int prot, int flags, vfs_node_t* node, uint64_t file_offset);
err_t  vma_remove(process_t* proc, uint64_t addr, size_t len);
vma_t* vma_find(process_t* proc, uint64_t addr);
int    vma_handle_fault(process_t* proc, uint64_t fault_addr, uint64_t error_code);
err_t  vma_duplicate(process_t* dst);
void   vma_cleanup(process_t* proc);
/* Atomically replace VMAs in [addr, addr+len) with a new VMA.
 * Removes overlapping VMAs and adds the new one under a single vma_lock
 * acquisition to prevent MAP_FIXED window races. */
vma_t* vma_replace(process_t* proc, uint64_t addr, size_t len,
                   int prot, int flags, vfs_node_t* node, uint64_t file_offset);

#endif
