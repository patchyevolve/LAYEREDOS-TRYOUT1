# ramdisk_blk.c — Writable Block-Device Ramdisk

**Path:** `os/src/kernel/ramdisk_blk.c`  
**Layer:** Layer 1 (HAL) — block device driver

---

## Purpose

Creates a 1 MB in-memory block device by allocating contiguous physical
pages from the PMM and registering them as a `block_dev_t` named `"ramdisk"`.
SFS can be formatted and mounted on top of this device, providing a fully
writable filesystem without any persistent disk.

---

## Size

```c
#define RAMDISK_BLK_SIZE  (1 * 1024 * 1024)   // 1 MB = 2048 × 512-byte sectors
```

1 MB requires `1MB / 4KB = 256` contiguous physical pages from `pmm_alloc_pages`.

---

## `ramdisk_blk_init`

1. `pmm_alloc_pages(256)` — allocates 256 contiguous pages.
2. `PHYS_TO_VIRT(phys)` — converts to a virtual address for kernel access.
3. `kmemset` the entire range to zero.
4. Fills `ramdisk_blk_dev`:
   - `name = "ramdisk"`
   - `block_count = 2048` (1 MB / 512 B)
   - `block_size = 512`
   - `read = ramdisk_blk_read`
   - `write = ramdisk_blk_write`
5. `block_register(&ramdisk_blk_dev)` — adds to the block device registry.

---

## `ramdisk_blk_read` / `ramdisk_blk_write`

Both translate `lba` to a byte offset (`lba × BLOCK_SIZE`) and perform a
`kmemcpy` into/from `ramdisk_blk_data`.  Bounds check: if
`offset + len > RAMDISK_BLK_SIZE`, returns `ERR_NOSPACE`.

---

## Interaction with SFS

The shell's `format` command calls:
```c
block_dev_t* bdev = block_find("ramdisk");
sfs_format(bdev);   // write SFS superblock + empty inode table
sfs_mount(bdev);    // parse superblock, register as VFS filesystem
```

After mounting, the shell can use `mkdir`, `writefile`, `ls`, `cat`,
`rm`, `run` against the SFS-formatted ramdisk.

---

## Persistence

The ramdisk contents exist only in RAM.  They are lost on reboot or
poweroff.  There is no mechanism to save the ramdisk to a persistent
disk — that would require a `save`/`load` command using ATA I/O.
