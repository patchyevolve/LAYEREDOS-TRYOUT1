# block.c — Block Device Registry

**Path:** `os/src/kernel/block.c`  
**Layer:** Layer 1/2 boundary — device abstraction

---

## Purpose

Maintains a global registry of block devices.  Any driver (ATA, ramdisk)
registers itself once; the filesystem layer (`sfs.c`) looks up a device
by name and issues reads/writes through the uniform `block_read`/`block_write`
interface without knowing the underlying driver.

---

## Internal state

```c
static block_dev_t block_devs[MAX_BLOCK_DEVICES];  // MAX = 8
static int block_dev_count = 0;
```

A flat array of up to 8 `block_dev_t` structs, copied by value on
registration.  No dynamic allocation — the structs are small and the
limit is sufficient for the current hardware set (one ramdisk + up to
four ATA drives).

---

## `block_register(dev)`

Copies `*dev` into the next free slot in `block_devs[]`.  Returns the
slot index (≥ 0) on success or -1 if the table is full.  Callers should
only register once per device; there is no deregister operation.

---

## `block_read` / `block_write`

Thin wrappers:

```c
err_t block_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    if (!dev || !dev->read) return ERR_INVAL;
    return dev->read(dev, lba, count, buf);
}
```

The function pointer `dev->read` is set by the driver at registration time.
For `ramdisk_blk`, it copies from a memory buffer.  For `ata`, it does PIO.
The caller always uses the same API regardless.

---

## `block_find(name)`

Linear scan of `block_devs[]` comparing `name` with `kstrcmp`.  Names are
set by the driver (e.g., `"ramdisk"` for `ramdisk_blk`).  Returns a pointer
to the `block_dev_t` slot in the global array — valid for the lifetime of
the kernel.

---

## Thread safety

No locking.  Block device registration happens at boot time (single-threaded),
and subsequent reads/writes are protected by the driver's own serialisation
(PIO is inherently single-threaded; ramdisk accesses are memory copies that
are atomic at the CPU level for aligned operations).  Concurrent block I/O
from multiple threads would need an external lock around `block_read`/`block_write`.
