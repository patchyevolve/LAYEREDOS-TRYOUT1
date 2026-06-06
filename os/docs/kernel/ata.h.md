# ata.h — ATA Driver Interface

**Path:** `os/src/kernel/ata.h`  
**Layer:** Layer 1 (HAL) — header

---

## Purpose

Declares the `ata_drive_t` struct and the public API for the ATA driver.
Included by `shell.c` (for the `atatest` command) and any future storage
layer that uses raw ATA I/O directly rather than going through the block
abstraction.

---

## `ata_drive_t`

```c
typedef struct {
    uint16_t  base;          // I/O base port (0x1F0 or 0x170)
    uint16_t  ctrl;          // control port (0x3F6 or 0x376)
    int       irq;           // IRQ line (14 or 15) — not yet used for IRQ-driven I/O
    int       present;       // 1 if drive responded to IDENTIFY
    uint64_t  sector_count;  // total LBA sectors (from IDENTIFY words 60/100)
    char      model[41];     // model string from IDENTIFY words 27-46 (null-terminated)
} ata_drive_t;
```

This struct is kept internal to `ata.c` (static array `drives[4]`).
External code accesses drive information through the accessor functions.

---

## Constants

```c
#define ATA_SECTORS     256   // max sectors per single transfer (historical limit)
#define ATA_SECTOR_SIZE 512   // bytes per sector
```

---

## API summary

| Function | Returns | Description |
|----------|---------|-------------|
| `ata_init()` | `err_t` | Detect all drives via IDENTIFY |
| `ata_read_sectors(drive, lba, count, buf)` | `int` (bytes or -1) | PIO read |
| `ata_write_sectors(drive, lba, count, buf)` | `int` (bytes or -1) | PIO write |
| `ata_drive_present(drive)` | `int` | 0/1 |
| `ata_drive_sectors(drive)` | `uint64_t` | Sector count |
| `ata_drive_model(drive)` | `const char*` | Model string |

`drive` parameter: 0 = primary master, 1 = primary slave, 2 = secondary master,
3 = secondary slave.  Values > 3 return 0/NULL/-1 safely.
