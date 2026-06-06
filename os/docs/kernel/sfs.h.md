# sfs.h — Simple Filesystem Interface

**Path:** `os/src/kernel/sfs.h`  
**Layer:** Layer 5 (Filesystem) — header

---

## Purpose

Declares the SFS on-disk data structures and the three public API functions.
Included by `shell.c` (`format`, `mount` commands) and any code that
needs to mount or query SFS.

---

## Constants

```c
#define SFS_MAGIC         0x53465301   // identifies SFS on-disk
#define SFS_BLOCK_SIZE    512          // must match BLOCK_SIZE
#define SFS_MAX_INODES    1024         // inode table size
#define SFS_NAME_MAX      28           // max filename chars in dirent
#define SFS_DIRECT_BLOCKS 12           // direct block pointers per inode
```

```c
#define SFS_TYPE_FREE  0   // unallocated inode
#define SFS_TYPE_FILE  1   // regular file
#define SFS_TYPE_DIR   2   // directory
```

---

## On-disk structures

All `__attribute__((packed))` to ensure byte-exact disk layout.

### `sfs_superblock_t` (512 bytes)

Layout descriptor for the entire filesystem.  Always at block 0.

### `sfs_inode_t` (32 bytes)

File/directory metadata: type, mode, size, block pointers.
Max 32 inodes per 512-byte block → `SFS_INODES_PER_BLOCK = 16`.

### `sfs_dirent_t` (32 bytes)

One directory entry: inode number + 28-char name.
Max 16 entries per 512-byte block → `SFS_DIRENTS_PER_BLOCK = 16`.

---

## `sfs_fs_t`

```c
typedef struct sfs_fs {
    block_dev_t*     bdev;       // underlying block device
    sfs_superblock_t sb;         // in-memory copy of superblock
    vfs_fs_t         vfs_fs;     // VFS filesystem descriptor
    vfs_node_t       root_node;  // VFS root node for "/"
} sfs_fs_t;
```

One `sfs_fs_t` is allocated per `sfs_mount` call via `kmalloc`.

---

## API

```c
err_t sfs_format(block_dev_t* bdev);
// Write SFS superblock, zero bitmaps, create root inode.
// Call before sfs_mount on a new/empty device.

err_t sfs_mount(block_dev_t* bdev);
// Read superblock, validate magic, register as active VFS filesystem.
// Returns ERR_INVAL if magic is wrong (not formatted).

int sfs_get_block_size(void);
// Returns SFS_BLOCK_SIZE (512). Informational.
```
