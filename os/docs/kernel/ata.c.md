# ata.c — ATA/IDE Block Device Driver

**Path:** `os/src/kernel/ata.c`  
**Layer:** Layer 1 (HAL) — device driver

---

## Purpose

Implements PIO-mode ATA (IDE) disk I/O for up to four drives (two channels,
master and slave each).  Provides sector-level read and write via the block
device abstraction.  Used by the block layer to back a persistent storage
device if QEMU is launched with a disk image.

---

## Hardware layout

| Channel | Base I/O | Control | IRQ | Drives |
|---------|---------|---------|-----|--------|
| Primary | `0x1F0` | `0x3F6` | 14 | 0 (master), 1 (slave) |
| Secondary | `0x170` | `0x376` | 15 | 2 (master), 3 (slave) |

The `drives[]` array indexes 0–3 map directly: drive 0 and 1 share the
primary channel, 2 and 3 share the secondary.  Within a channel,
`drive & 1` selects master (0) or slave (1).

---

## ATA register offsets (relative to base)

| Offset | Name | Read | Write |
|--------|------|------|-------|
| +0 | DATA | 16-bit data | 16-bit data |
| +1 | ERROR / FEATURES | Error flags | Feature select |
| +2 | SECTOR COUNT | Sectors to transfer | Sectors to transfer |
| +3–5 | LBA 0–2 | LBA bits 0–23 | LBA bits 0–23 |
| +6 | DRIVE | Drive/head select | Drive/head select |
| +7 | STATUS / COMMAND | Status byte | Command byte |

---

## `ata_init`

1. Zeroes the `drives[4]` array.
2. Sets base/ctrl/irq for all four drive slots.
3. Calls `ata_identify_drive` for each — sends `IDENTIFY` (0xEC), reads
   the 256-word response, extracts sector count (28-bit from words 60–61,
   48-bit from words 100–103) and model string (words 27–46, byte-swapped).
4. Prints a summary for any present drive.

---

## `ata_pio_transfer` (internal)

Core read/write routine:

```
1. Wait for BSY clear (up to 10000 poll iterations)
2. Select drive: outb(DRIVE, 0xA0 for master | 0xB0 for slave)
3. Write sector count + LBA[0..2] registers
4. Write command: 0x20 (READ_PIO) or 0x30 (WRITE_PIO)
5. For each sector:
   a. Wait BSY clear + DRQ set
   b. Transfer 256 16-bit words via inw/outw DATA register
6. For writes: send FLUSH (0xE7) and wait for BSY clear
```

PIO is slow (CPU-bound, one word at a time) but simple — no DMA setup,
no interrupt handler needed for basic operation.  For the current use
case (booting, loading programs) it is sufficient.

---

## Error handling

- `ata_wait_bsy` times out after 10000 polls → returns -1 → transfer fails.
- `ata_wait_drq` also checks the `ATA_STATUS_ERR` bit — if set, the drive
  reported an error, and the function returns -1 without reading garbage data.
- `ata_identify_drive` silently returns if the status byte is 0 (no drive)
  or if LBA mid/hi bytes are non-zero (ATAPI / non-ATA device).

---

## Public API

| Function | Description |
|----------|-------------|
| `ata_init()` | Detect and identify all drives |
| `ata_read_sectors(drive, lba, count, buf)` | PIO read; returns bytes read or -1 |
| `ata_write_sectors(drive, lba, count, buf)` | PIO write; returns bytes written or -1 |
| `ata_drive_present(drive)` | Boolean: 1 if drive N was detected |
| `ata_drive_sectors(drive)` | Total sector count |
| `ata_drive_model(drive)` | Model string (null-terminated, trimmed) |

---

## LBA28 limitation

The current driver only writes LBA bits 0–23 (3 bytes).  Full LBA48
addressing (which allows > 128 GB) is detected (48-bit sector count is
read from IDENTIFY) but the transfer command is still `READ_PIO`/`WRITE_PIO`
(LBA28 commands).  For the 512 MB QEMU VM this is fine; disks larger than
128 GB would need LBA48 commands (0x24/0x34).
