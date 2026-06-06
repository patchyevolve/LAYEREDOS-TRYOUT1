# ramdisk_blk.h — Writable Block Ramdisk Interface

**Path:** `os/src/kernel/ramdisk_blk.h`  
**Layer:** Layer 1 (HAL) — header

---

## Purpose

Declares the single initialisation function for the block-device ramdisk.

---

## API

```c
err_t ramdisk_blk_init(void);
// Allocate 1 MB from PMM, register as "ramdisk" block device.
// Must be called after pmm_init() and before sfs_mount().
// Returns ERR_OK or ERR_NOMEM if PMM cannot provide 256 contiguous pages.
```

---

## Usage sequence

```c
pmm_init(...);
ramdisk_blk_init();         // creates the block device
block_dev_t* d = block_find("ramdisk");
sfs_format(d);              // write SFS metadata
sfs_mount(d);               // mount as active VFS filesystem
```

After this sequence the shell commands `ls`, `cat`, `writefile`, `mkdir`,
`rm`, `run` all work against the SFS on the ramdisk.
