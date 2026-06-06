#include "kernel.h"
#include "ata.h"
#include "hal.h"

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

static ata_drive_t drives[4];

static int ata_wait_bsy(uint16_t base, int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (!(inb(base + ATA_REG_STATUS) & ATA_STATUS_BSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(uint16_t base, int timeout) {
    for (int i = 0; i < timeout; i++) {
        uint8_t status = inb(base + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
    }
    return -1;
}

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

static int ata_pio_transfer(uint16_t base, uint8_t drive, uint64_t lba,
                            uint8_t count, void* buf, int write) {
    if (ata_wait_bsy(base, 10000)) return -1;

    outb(base + ATA_REG_DRIVE, drive ? 0xB0 : 0xA0);
    outb(base + ATA_REG_SECCOUNT, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba));
    outb(base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(base + ATA_REG_CMD, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    uint16_t* word_buf = (uint16_t*)buf;

    for (int s = 0; s < count; s++) {
        if (ata_wait_bsy(base, 10000)) return -1;
        if (ata_wait_drq(base, 10000)) return -1;

        if (write) {
            for (int i = 0; i < 256; i++)
                outw(base + ATA_REG_DATA, word_buf[s * 256 + i]);
        } else {
            for (int i = 0; i < 256; i++)
                word_buf[s * 256 + i] = inw(base + ATA_REG_DATA);
        }
    }

    if (write) {
        outb(base + ATA_REG_CMD, ATA_CMD_FLUSH);
        ata_wait_bsy(base, 10000);
    }

    return count * ATA_SECTOR_SIZE;
}

int ata_read_sectors(uint8_t drive, uint64_t lba, uint8_t count, void* buf) {
    if (drive > 3 || !drives[drive].present) return -1;
    return ata_pio_transfer(drives[drive].base, drive & 1, lba, count, buf, 0);
}

int ata_write_sectors(uint8_t drive, uint64_t lba, uint8_t count, const void* buf) {
    if (drive > 3 || !drives[drive].present) return -1;
    return ata_pio_transfer(drives[drive].base, drive & 1, lba, count, (void*)buf, 1);
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
