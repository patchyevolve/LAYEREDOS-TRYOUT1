#include "kernel.h"
#include "block.h"
#include "pmm.h"

#define RAMDISK_BLK_SIZE (1 * 1024 * 1024)

static uint8_t* ramdisk_blk_data = NULL;
static block_dev_t ramdisk_blk_dev;

static err_t ramdisk_blk_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    (void)dev;
    uint64_t offset = lba * BLOCK_SIZE;
    uint64_t len = (uint64_t)count * BLOCK_SIZE;
    if (offset + len > RAMDISK_BLK_SIZE) return ERR_NOSPACE;
    kmemcpy(buf, ramdisk_blk_data + offset, len);
    return ERR_OK;
}

static err_t ramdisk_blk_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    (void)dev;
    uint64_t offset = lba * BLOCK_SIZE;
    uint64_t len = (uint64_t)count * BLOCK_SIZE;
    if (offset + len > RAMDISK_BLK_SIZE) return ERR_NOSPACE;
    kmemcpy(ramdisk_blk_data + offset, buf, len);
    return ERR_OK;
}

err_t ramdisk_blk_init(void) {
    uint64_t pages = (RAMDISK_BLK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return ERR_NOMEM;
    ramdisk_blk_data = (uint8_t*)PHYS_TO_VIRT(phys);
    kmemset(ramdisk_blk_data, 0, RAMDISK_BLK_SIZE);

    kstrncpy(ramdisk_blk_dev.name, "ramdisk", sizeof(ramdisk_blk_dev.name) - 1);
    ramdisk_blk_dev.block_count = RAMDISK_BLK_SIZE / BLOCK_SIZE;
    ramdisk_blk_dev.block_size = BLOCK_SIZE;
    ramdisk_blk_dev.read = ramdisk_blk_read;
    ramdisk_blk_dev.write = ramdisk_blk_write;
    ramdisk_blk_dev.private_data = NULL;

    block_register(&ramdisk_blk_dev);

    kprintf("[RAMDISK_BLK] %u KB writable block device at %p\n",
            RAMDISK_BLK_SIZE / 1024, ramdisk_blk_data);
    return ERR_OK;
}
