#include "kernel.h"
#include "snap.h"
#include "sfs.h"
#include "block.h"
#include "pmm.h"
#include "kmalloc.h"
#include "hal.h"
#include "sync.h"

#define SNAP_SIZE(snap) ((snap)->blocks * BLOCK_SIZE)
#define SNAP_PAGES(snap) ((SNAP_SIZE(snap) + PAGE_SIZE - 1) / PAGE_SIZE)

typedef struct {
    int        used;
    uint32_t   blocks;
    uint64_t   timestamp;
    char       desc[SNAP_DESC_LEN];
    uint8_t*   data;
} snap_entry_t;

static snap_entry_t snapshots[SNAP_MAX];
static spinlock_t snap_lock;

int snapshot_count(void) {
    int n = 0;
    cpu_flags_t _sf;
    spinlock_acquire(&snap_lock, &_sf);
    for (int i = 0; i < SNAP_MAX; i++)
        if (snapshots[i].used) n++;
    spinlock_release(&snap_lock, _sf);
    return n;
}

int snapshot_info(int index, snap_info_t* info) {
    cpu_flags_t _sf;
    spinlock_acquire(&snap_lock, &_sf);
    if (index < 0 || index >= SNAP_MAX || !snapshots[index].used || !info) {
        spinlock_release(&snap_lock, _sf);
        return -1;
    }
    snap_entry_t* s = &snapshots[index];
    info->used = 1;
    info->blocks = s->blocks;
    info->timestamp = s->timestamp;
    kstrncpy(info->desc, s->desc, SNAP_DESC_LEN - 1);
    spinlock_release(&snap_lock, _sf);
    return 0;
}

void snapshot_free(int index) {
    cpu_flags_t _sf;
    spinlock_acquire(&snap_lock, &_sf);
    if (index < 0 || index >= SNAP_MAX || !snapshots[index].used) {
        spinlock_release(&snap_lock, _sf);
        return;
    }
    snap_entry_t* s = &snapshots[index];
    uint32_t pages = SNAP_PAGES(s);
    uint64_t phys = VIRT_TO_PHYS((uint64_t)s->data);
    kmemset(s, 0, sizeof(snap_entry_t));
    spinlock_release(&snap_lock, _sf);
    if (phys) pmm_free_pages(phys, pages);
}

err_t snapshot_take(block_dev_t* bdev, const char* desc) {
    if (!bdev) return ERR_INVAL;

    block_sync_dev(bdev);

    uint32_t total = (uint32_t)bdev->block_count;
    uint32_t size = total * BLOCK_SIZE;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return ERR_NOMEM;
    uint8_t* buf = (uint8_t*)PHYS_TO_VIRT(phys);

    for (uint32_t i = 0; i < total; i++) {
        if (block_read(bdev, i, 1, buf + i * BLOCK_SIZE) != ERR_OK) {
            pmm_free_pages(phys, pages);
            return ERR_IO;
        }
    }

    cpu_flags_t _sf;
    spinlock_acquire(&snap_lock, &_sf);
    int slot = -1;
    for (int i = 0; i < SNAP_MAX; i++) {
        if (!snapshots[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        uint32_t old_pages = SNAP_PAGES(&snapshots[0]);
        uint64_t old_phys = VIRT_TO_PHYS((uint64_t)snapshots[0].data);
        kmemset(&snapshots[0], 0, sizeof(snap_entry_t));
        if (old_phys) pmm_free_pages(old_phys, old_pages);
    }
    snapshots[slot].used = 1;
    snapshots[slot].blocks = total;
    snapshots[slot].timestamp = hal_timer_get_ticks();
    snapshots[slot].data = buf;
    kstrncpy(snapshots[slot].desc, desc ? desc : "", SNAP_DESC_LEN - 1);
    spinlock_release(&snap_lock, _sf);

    kprintf("[SNAP] Snapshot %d taken: %u blocks (%u KB) '%s'\n",
            slot, total, total / 2, snapshots[slot].desc);
    return ERR_OK;
}

err_t snapshot_rollback(block_dev_t* bdev, int index) {
    if (!bdev) return ERR_INVAL;
    cpu_flags_t _sf;
    spinlock_acquire(&snap_lock, &_sf);
    if (index < 0 || index >= SNAP_MAX || !snapshots[index].used) {
        spinlock_release(&snap_lock, _sf);
        return ERR_INVAL;
    }
    snap_entry_t* s = &snapshots[index];
    spinlock_release(&snap_lock, _sf);

    kprintf("[SNAP] Rolling back to snapshot %d (%u blocks) '%s'...\n",
            index, s->blocks, s->desc);

    for (uint32_t i = 0; i < s->blocks; i++) {
        if (block_write(bdev, i, 1, s->data + i * BLOCK_SIZE) != ERR_OK)
            return ERR_IO;
    }

    block_sync_dev(bdev);

    err_t e = sfs_mount(bdev);
    if (e) {
        kprintf("[SNAP] Rollback done, but remount failed (%d)\n", e);
        return e;
    }

    kprintf("[SNAP] Rollback complete, filesystem remounted\n");
    return ERR_OK;
}
