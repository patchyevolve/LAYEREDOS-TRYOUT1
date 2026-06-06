# ramdisk.h — VFS Ramdisk Interface

**Path:** `os/src/kernel/ramdisk.h`  
**Layer:** Layer 5 (Filesystem) — header

---

## Purpose

Declares the ramdisk types and the two public functions.

---

## Constants

```c
#define RAMDISK_MAX_FILES  64    // max files in the ramdisk
#define RAMDISK_NAME_MAX   64    // max filename length
```

---

## `ramdisk_file_t`

```c
typedef struct {
    char     name[64];   // filename
    uint8_t* data;       // pointer to file content (not owned — caller's data)
    uint64_t size;       // content size in bytes
} ramdisk_file_t;
```

---

## API

```c
err_t ramdisk_init(void);
// Mount the ramdisk as the VFS root filesystem.
// Call once after vfs_init().

int ramdisk_add_file(const char* name, const void* data, uint64_t size);
// Register a file.  data pointer must remain valid for kernel lifetime.
// Returns 0 on success, -1 if table is full.
```

---

## Relationship to `ramdisk_blk`

| Feature | `ramdisk` (this file) | `ramdisk_blk` |
|---------|----------------------|---------------|
| Backed by | Static memory pointers | PMM-allocated pages |
| VFS? | Yes (VFS filesystem) | Via SFS on top |
| Writable? | No | Yes |
| Purpose | Boot-time read-only files | Persistent formatted storage (SFS) |
