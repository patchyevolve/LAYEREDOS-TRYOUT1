#ifndef ATA_H
#define ATA_H

#include "types.h"
#include "sched.h"

#define ATA_SECTORS 256
#define ATA_SECTOR_SIZE 512

typedef struct {
    uint16_t         base;
    uint16_t         ctrl;
    int              irq;
    int              present;
    uint64_t         sector_count;
    char             model[41];
    wait_queue_t     wq;
    volatile uint8_t irq_status;
    volatile int     irq_received;
} ata_drive_t;

err_t ata_init(void);
int   ata_read_sectors(uint8_t drive, uint64_t lba, uint8_t count, void* buf);
int   ata_write_sectors(uint8_t drive, uint64_t lba, uint8_t count, const void* buf);
int   ata_drive_present(uint8_t drive);
uint64_t ata_drive_sectors(uint8_t drive);
const char* ata_drive_model(uint8_t drive);
err_t ata_blk_init(void);

#endif
