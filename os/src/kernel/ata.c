#include "kernel.h"
#include "ata.h"
#include "hal.h"
#include "block.h"
#include "sched.h"
#include "sync.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_PRIMARY_IRQ   14
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_SECONDARY_IRQ 15

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_DRIVE     6
#define ATA_REG_CMD       7
#define ATA_REG_STATUS    7

#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_FLUSH     0xE7

#define ATA_STATUS_ERR    0x01
#define ATA_STATUS_DRQ    0x08
#define ATA_STATUS_SRV    0x10
#define ATA_STATUS_DF     0x20
#define ATA_STATUS_RDY    0x40
#define ATA_STATUS_BSY    0x80

#define ATA_IRQ_TIMEOUT_MS 5000

static ata_drive_t drives[4];
static mutex_t   ata_global_lock;

static int ata_wait_bsy(uint16_t base, int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (!(inb(base + ATA_REG_STATUS) & ATA_STATUS_BSY))
            return 0;
    }
    return -1;
}

/* IRQ handler for ATA primary/secondary interrupts */
static void ata_irq_handler(int_frame_t* frame, void* data) {
    (void)frame;
    uint8_t drive_idx = (uint8_t)(uintptr_t)data;
    if (drive_idx >= 4) return;

    drives[drive_idx].irq_status = inb(drives[drive_idx].base + ATA_REG_STATUS);
    drives[drive_idx].irq_received = 1;
    sched_wake(&drives[drive_idx].wq);
}

/* Polling-based IDENTIFY for use during init before IRQs are unmasked */
static void ata_identify_drive(ata_drive_t* d, uint16_t base, uint8_t drive) {
    outb(base + ATA_REG_DRIVE, drive ? 0xB0 : 0xA0);
    outb(base + ATA_REG_SECCOUNT, 0);
    outb(base + ATA_REG_LBA0, 0);
    outb(base + ATA_REG_LBA1, 0);
    outb(base + ATA_REG_LBA2, 0);
    outb(base + ATA_REG_CMD, ATA_CMD_IDENTIFY);

    uint8_t status = inb(base + ATA_REG_STATUS);
    if (!status) return;

    if (ata_wait_bsy(base, 10000)) return;

    uint8_t lbamid = inb(base + ATA_REG_LBA1);
    uint8_t lbahi  = inb(base + ATA_REG_LBA2);

    if (lbamid || lbahi) {
        d->present = 1;
    } else {
        if (!(inb(base + ATA_REG_STATUS) & ATA_STATUS_DRQ))
            return;
    }

    uint16_t buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = inw(base + ATA_REG_DATA);

    if (!d->present) {
        d->present = 1;
    }

    uint32_t sectors_28 = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
    uint32_t sectors_48_lo = (uint32_t)buf[100] | ((uint32_t)buf[101] << 16);
    uint32_t sectors_48_hi = (uint32_t)buf[102] | ((uint32_t)buf[103] << 16);
    d->sector_count = sectors_48_lo ? ((uint64_t)sectors_48_hi << 32 | sectors_48_lo)
                                    : sectors_28;

    int i;
    for (i = 0; i < 40; i += 2) {
        d->model[i]     = buf[27 + i/2] >> 8;
        d->model[i + 1] = buf[27 + i/2] & 0xFF;
    }
    d->model[40] = '\0';

    i = 39;
    while (i > 0 && d->model[i] == ' ') d->model[i--] = '\0';
}

err_t ata_init(void) {
    kmemset(drives, 0, sizeof(drives));
    mutex_init(&ata_global_lock);

    drives[0].base = ATA_PRIMARY_IO;
    drives[0].ctrl = ATA_PRIMARY_CTRL;
    drives[0].irq  = ATA_PRIMARY_IRQ;
    drives[1].base = ATA_PRIMARY_IO;
    drives[1].ctrl = ATA_PRIMARY_CTRL;
    drives[1].irq  = ATA_PRIMARY_IRQ;
    drives[2].base = ATA_SECONDARY_IO;
    drives[2].ctrl = ATA_SECONDARY_CTRL;
    drives[2].irq  = ATA_SECONDARY_IRQ;
    drives[3].base = ATA_SECONDARY_IO;
    drives[3].ctrl = ATA_SECONDARY_CTRL;
    drives[3].irq  = ATA_SECONDARY_IRQ;

    for (int i = 0; i < 4; i++) {
        wait_queue_init(&drives[i].wq);
        drives[i].irq_received = 0;
        drives[i].irq_status = 0;
    }

    /* Register IRQ handlers */
    hal_irq_register(ATA_PRIMARY_IRQ, ata_irq_handler, (void*)(uintptr_t)0);
    hal_irq_register(ATA_PRIMARY_IRQ, ata_irq_handler, (void*)(uintptr_t)1);
    hal_irq_register(ATA_SECONDARY_IRQ, ata_irq_handler, (void*)(uintptr_t)2);
    hal_irq_register(ATA_SECONDARY_IRQ, ata_irq_handler, (void*)(uintptr_t)3);

    ata_identify_drive(&drives[0], ATA_PRIMARY_IO, 0);
    ata_identify_drive(&drives[1], ATA_PRIMARY_IO, 1);
    ata_identify_drive(&drives[2], ATA_SECONDARY_IO, 0);
    ata_identify_drive(&drives[3], ATA_SECONDARY_IO, 1);

    for (int i = 0; i < 4; i++) {
        if (drives[i].present) {
            kprintf("[ATA] Drive %d: %s (%llu sectors, %llu MB)\n",
                    i, drives[i].model,
                    drives[i].sector_count,
                    drives[i].sector_count / 2048);
        }
    }
    return ERR_OK;
}

/* Wait for DRQ or error by polling status (no IRQ dependency).
   Returns 0 on DRQ ready, -1 on error, -2 on timeout. */
static int ata_poll_drq(uint16_t base, uint64_t timeout_ms) {
    uint64_t deadline = hal_timer_get_ticks()
              + timeout_ms * hal_timer_get_hz() / 1000;
    for (;;) {
        uint8_t s = inb(base + ATA_REG_STATUS);
        if (s & ATA_STATUS_DRQ)   return 0;
        if (s & ATA_STATUS_ERR)   return -1;
        if (hal_timer_get_ticks() > deadline) return -2;
        for (volatile int _ = 0; _ < 100; _++) asm volatile("pause");
    }
}

/* PIO transfer using pure polling — no IRQ or scheduler dependency.
   The caller must hold ata_global_lock. */
static int ata_pio_poll(uint16_t base, uint8_t drive, uint64_t lba,
                        uint8_t count, void* buf, int write) {
    uint16_t* word_buf = (uint16_t*)buf;
    int sector_words = ATA_SECTOR_SIZE / 2;

    /* Select drive with LBA mode enabled (bit 6) and drive select (bit 4) */
    outb(base + ATA_REG_DRIVE, 0xE0 | (drive & 1));

    /* Wait for BSY=0 before issuing command */
    for (int w = 0; w < 100000; w++) {
        if (!(inb(base + ATA_REG_STATUS) & ATA_STATUS_BSY)) break;
    }

    /* Set sector count and LBA bits (LBA28) */
    outb(base + ATA_REG_SECCOUNT, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba));
    outb(base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(base + ATA_REG_LBA2, (uint8_t)(lba >> 16));

    /* Send command */
    outb(base + ATA_REG_CMD, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    for (int s = 0; s < count; s++) {
        if (ata_poll_drq(base, ATA_IRQ_TIMEOUT_MS) != 0)
            return -1;

        for (int i = 0; i < sector_words; i++) {
            if (write)
                outw(base + ATA_REG_DATA, word_buf[s * sector_words + i]);
            else
                word_buf[s * sector_words + i] = inw(base + ATA_REG_DATA);
        }
    }

    if (write) {
        /* Flush write cache */
        outb(base + ATA_REG_CMD, ATA_CMD_FLUSH);
    }

    /* Final status check */
    uint8_t status = inb(base + ATA_REG_STATUS);
    if (status & ATA_STATUS_ERR) return -1;
    return count * ATA_SECTOR_SIZE;
}

/* IRQ-driven PIO transfer with polling fallback */
static int ata_pio_transfer_irq(uint16_t base, uint8_t drive, uint64_t lba,
                                uint8_t count, void* buf, int write) {
    err_t e = mutex_lock(&ata_global_lock, 10000);
    if (e != ERR_OK) return -1;

    uint8_t drive_idx;
    for (drive_idx = 0; drive_idx < 4; drive_idx++) {
        if (drives[drive_idx].base == base && (drive_idx & 1) == (drive & 1))
            break;
    }
    if (drive_idx >= 4) {
        mutex_unlock(&ata_global_lock);
        return -1;
    }

    ata_drive_t* drv = &drives[drive_idx];

    /* Use pure polling path — IRQ-driven PIO is unreliable on KVM where
     * the I/O APIC is inaccessible and legacy PIC may not deliver ATA
     * interrupts through the local APIC's ExtINTA mechanism. */
    outb(drv->ctrl, 0x02); /* set nIEN to mask ATA interrupts */
    int ret = ata_pio_poll(base, drive, lba, count, buf, write);

    mutex_unlock(&ata_global_lock);
    return ret;
}

int ata_read_sectors(uint8_t drive, uint64_t lba, uint8_t count, void* buf) {
    if (drive > 3 || !drives[drive].present) return -1;
    return ata_pio_transfer_irq(drives[drive].base, drive & 1, lba, count, buf, 0);
}

int ata_write_sectors(uint8_t drive, uint64_t lba, uint8_t count, const void* buf) {
    if (drive > 3 || !drives[drive].present) return -1;
    return ata_pio_transfer_irq(drives[drive].base, drive & 1, lba, count, (void*)buf, 1);
}

int ata_drive_present(uint8_t drive) {
    return (drive < 4) ? drives[drive].present : 0;
}

uint64_t ata_drive_sectors(uint8_t drive) {
    return (drive < 4) ? drives[drive].sector_count : 0;
}

const char* ata_drive_model(uint8_t drive) {
    return (drive < 4) ? drives[drive].model : NULL;
}

/* ---- Block device wrapper ---- */

static err_t ata_blk_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    uint8_t drive_idx = (uint8_t)(uintptr_t)dev->private_data;
    int ret = ata_read_sectors(drive_idx, lba, count, buf);
    return (ret < 0) ? ERR_IO : ERR_OK;
}

static err_t ata_blk_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    uint8_t drive_idx = (uint8_t)(uintptr_t)dev->private_data;
    int ret = ata_write_sectors(drive_idx, lba, count, buf);
    return (ret < 0) ? ERR_IO : ERR_OK;
}

err_t ata_blk_init(void) {
    for (int i = 0; i < 4; i++) {
        if (!drives[i].present) continue;

        block_dev_t bd;
        kstrncpy(bd.name, "ata", sizeof(bd.name) - 1);

        char suffix[4] = { '0' + (char)i, '\0' };
        kstrncat(bd.name, suffix, sizeof(bd.name) - kstrlen(bd.name) - 1);

        bd.block_count = drives[i].sector_count;
        bd.block_size = ATA_SECTOR_SIZE;
        bd.read = ata_blk_read;
        bd.write = ata_blk_write;
        bd.private_data = (void*)(uintptr_t)i;

        int idx = block_register(&bd);
        if (idx >= 0) {
            kprintf("[ATA_BLK] Registered drive %d as '%s' (%llu blocks)\n",
                    i, bd.name, bd.block_count);
        }
    }
    return ERR_OK;
}
