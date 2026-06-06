# block.h — Block Device Abstraction Interface

**Path:** `os/src/kernel/block.h`  
**Layer:** Layer 1/2 boundary — header

---

## Purpose

Defines the `block_dev_t` struct and the uniform block I/O API.  Any
code that needs to read or write sectors should use this header, never
a specific driver header.  This is the kernel's equivalent of Linux's
`block_device` abstraction.

---

## Constants

```c
#define BLOCK_SIZE  512    // bytes per logical block (one ATA sector)
#define BLOCK_SHIFT 9      // log2(BLOCK_SIZE) — for fast multiply/divide
```

All block addresses (`lba`) are in units of `BLOCK_SIZE` bytes.

---

## `block_dev_t`

```c
typedef struct block_dev {
    char          name[16];       // device name ("ramdisk", "ata0", etc.)
    uint64_t      block_count;    // total number of BLOCK_SIZE blocks
    uint32_t      block_size;     // should always be BLOCK_SIZE (512)
    block_read_t  read;           // driver read function pointer
    block_write_t write;          // driver write function pointer
    void*         private_data;   // driver-specific state (unused for ramdisk)
} block_dev_t;
```

`block_read_t` and `block_write_t` are function pointer types:

```c
typedef err_t (*block_read_t)(block_dev_t* dev, uint64_t lba,
                               uint8_t count, void* buf);
typedef err_t (*block_write_t)(block_dev_t* dev, uint64_t lba,
                                uint8_t count, const void* buf);
```

`count` is in sectors (1–255).  `buf` must be at least `count * BLOCK_SIZE`
bytes.

---

## API summary

| Function | Description |
|----------|-------------|
| `block_read(dev, lba, count, buf)` | Call `dev->read` |
| `block_write(dev, lba, count, buf)` | Call `dev->write` |
| `block_register(dev)` | Copy `*dev` into registry; return index |
| `block_count()` | Number of registered devices |
| `block_get(index)` | Return device by index |
| `block_find(name)` | Return device by name |
