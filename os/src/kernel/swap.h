#ifndef SWAP_H
#define SWAP_H

#include "types.h"

/* Swap entry stored in PTE when Present=0.
 * Bit 0 = 0 (Present clear)
 * Bit 1 = 1 (swap marker, different from other non-present)
 * Bits 12-51: swap slot number (40 bits) */
#define SWAP_PTE_MARKER  (1ULL << 1)

/* Maximum swap slots */
#define SWAP_MAX_SLOTS 1024
#define SWAP_SLOT_SIZE PAGE_SIZE

err_t swap_init(void);
int   swap_alloc_slot(void);
void  swap_free_slot(int slot);
err_t swap_out(int slot, uint64_t phys_addr);
err_t swap_in(int slot, uint64_t phys_addr);
int   swap_used_slots(void);
int   swap_total_slots(void);

/* Encode/decode swap slot number into/from a PTE.
 * Bits 2-11 are reserved for original PTE flags (USER, WRITE, etc.). */
#define SWAP_PTE_FLAGS_MASK 0xFFC
static inline uint64_t swap_encode_pte(int slot, uint64_t orig_flags) {
    return ((uint64_t)slot << 12) | SWAP_PTE_MARKER | (orig_flags & SWAP_PTE_FLAGS_MASK);
}

static inline int swap_decode_pte(uint64_t pte) {
    return (int)((pte >> 12) & 0xFFFFFFFFFFULL);
}

static inline uint64_t swap_decode_pte_flags(uint64_t pte) {
    return pte & SWAP_PTE_FLAGS_MASK;
}

#endif