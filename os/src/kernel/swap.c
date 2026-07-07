#include "swap.h"
#include "pmm.h"
#include "kernel.h"
#include "sync.h"

/* Swap bitmap: 1 bit per slot, 1 = free */
static uint8_t swap_bitmap[SWAP_MAX_SLOTS / 8];
static int swap_nslots = 0;
static uint64_t swap_pool_phys = 0; /* physical address of swap backing pages */
static int swap_initialized = 0;
static spinlock_t swap_lock;

err_t swap_init(void) {
    spinlock_init(&swap_lock, "swap_lock");
    /* Allocate physical pages for swap backing store.
     * Use 4 pages = 16KB = 4 swap slots for now. */
    swap_nslots = 4;
    swap_pool_phys = pmm_alloc_pages(swap_nslots);
    if (!swap_pool_phys) {
        kprintf("[SWAP] Failed to allocate backing store\n");
        return ERR_NOMEM;
    }

    /* Mark all slots as free */
    kmemset(swap_bitmap, 0xFF, sizeof(swap_bitmap));

    /* Mark slots beyond our count as used */
    for (int i = swap_nslots; i < SWAP_MAX_SLOTS; i++) {
        swap_bitmap[i / 8] &= ~(1 << (i % 8));
    }

    kprintf("[SWAP] Initialized: %d slots, backing at phys %llx\n",
            swap_nslots, swap_pool_phys);
    swap_initialized = 1;
    return ERR_OK;
}

int swap_alloc_slot(void) {
    if (!swap_initialized) return -1;
    cpu_flags_t _sf;
    spinlock_acquire(&swap_lock, &_sf);
    for (int i = 0; i < swap_nslots; i++) {
        if (swap_bitmap[i / 8] & (1 << (i % 8))) {
            swap_bitmap[i / 8] &= ~(1 << (i % 8));
            spinlock_release(&swap_lock, _sf);
            /* Clear the backing store page */
            uint64_t page_phys = swap_pool_phys + (uint64_t)i * PAGE_SIZE;
            kmemset((void*)PHYS_TO_VIRT(page_phys), 0, PAGE_SIZE);
            return i;
        }
    }
    spinlock_release(&swap_lock, _sf);
    return -1; /* no free slots */
}

void swap_free_slot(int slot) {
    if (!swap_initialized || slot < 0 || slot >= swap_nslots) return;
    cpu_flags_t _sf;
    spinlock_acquire(&swap_lock, &_sf);
    swap_bitmap[slot / 8] |= (1 << (slot % 8));
    spinlock_release(&swap_lock, _sf);
}

err_t swap_out(int slot, uint64_t phys_addr) {
    if (!swap_initialized) return ERR_GENERAL;
    if (slot < 0 || slot >= swap_nslots) return ERR_INVAL;
    if (!phys_addr) return ERR_INVAL;
    cpu_flags_t _sf;
    spinlock_acquire(&swap_lock, &_sf);
    if (swap_bitmap[slot / 8] & (1 << (slot % 8))) {
        spinlock_release(&swap_lock, _sf);
        return ERR_INVAL;
    }
    spinlock_release(&swap_lock, _sf);

    uint64_t swap_page_phys = swap_pool_phys + (uint64_t)slot * PAGE_SIZE;
    kmemcpy((void*)PHYS_TO_VIRT(swap_page_phys),
            (void*)PHYS_TO_VIRT(phys_addr), PAGE_SIZE);
    return ERR_OK;
}

err_t swap_in(int slot, uint64_t phys_addr) {
    if (!swap_initialized) return ERR_GENERAL;
    if (slot < 0 || slot >= swap_nslots) return ERR_INVAL;
    if (!phys_addr) return ERR_INVAL;

    uint64_t swap_page_phys = swap_pool_phys + (uint64_t)slot * PAGE_SIZE;
    kmemcpy((void*)PHYS_TO_VIRT(phys_addr),
            (void*)PHYS_TO_VIRT(swap_page_phys), PAGE_SIZE);
    return ERR_OK;
}

int swap_used_slots(void) {
    if (!swap_initialized) return 0;
    int used = 0;
    cpu_flags_t _sf;
    spinlock_acquire(&swap_lock, &_sf);
    for (int i = 0; i < swap_nslots; i++) {
        if (!(swap_bitmap[i / 8] & (1 << (i % 8))))
            used++;
    }
    spinlock_release(&swap_lock, _sf);
    return used;
}

int swap_total_slots(void) {
    return swap_nslots;
}