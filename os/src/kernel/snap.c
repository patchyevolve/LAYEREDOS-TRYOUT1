#include "kernel.h"
#include "snap.h"
#include "sfs.h"
#include "block.h"
#include "pmm.h"
#include "kmalloc.h"

static uint8_t* snapshot_data = NULL;
static uint32_t snapshot_blocks = 0;

err_t snapshot_take(block_dev_t* bdev) {
    if (snapshot_data) {
        kfree(snapshot_data);
        snapshot_data = NULL;
        snapshot_blocks = 0;
    }

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

    snapshot_data = buf;
    snapshot_blocks = total;

    kprintf("[SNAP] Snapshot taken: %u blocks (%u KB)\n", total, total / 2);
    return ERR_OK;
}

err_t snapshot_rollback(block_dev_t* bdev) {
    if (!snapshot_data) return ERR_INVAL;

    kprintf("[SNAP] Rolling back to snapshot (%u blocks)...\n", snapshot_blocks);

    for (uint32_t i = 0; i < snapshot_blocks; i++) {
        if (block_write(bdev, i, 1, snapshot_data + i * BLOCK_SIZE) != ERR_OK)
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

int snapshot_exists(void) {
    return snapshot_data != NULL;
}
