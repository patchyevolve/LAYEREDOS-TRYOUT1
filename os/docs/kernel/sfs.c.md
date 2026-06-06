# sfs.c — Simple Filesystem

**Path:** `os/src/kernel/sfs.c`  
**Layer:** Layer 5 (Filesystem)

---

## Purpose

Implements SFS (Simple Filesystem) — a custom block-structured filesystem
with inodes, bitmaps, and directory entries.  Provides full create, read,
write, readdir, stat, truncate, and unlink operations through the VFS
interface.  Runs on any `block_dev_t` — in practice always the `ramdisk_blk`
block device.

---

## On-disk layout

All structures are 512-byte aligned (one block each where possible).
The layout is computed dynamically from `sfs_format` based on `total_blocks`:

```
Block 0:           Superblock (sfs_superblock_t, 512 bytes)
Block 1..ib_end:   Inode bitmap (one bit per inode, packed)
Block ib_end..bb_end: Block bitmap (one bit per data block)
Block bb_end..it_end: Inode table (sfs_inode_t entries, 16 bytes each)
Block it_end..:    Data blocks
```

The superblock records all layout offsets so `sfs_mount` can reconstruct
the layout without hardcoded assumptions.

---

## `sfs_superblock_t` (512 bytes)

| Field | Description |
|-------|-------------|
| `magic` | `0x53465301` — identifies SFS |
| `total_inodes` | Fixed at `SFS_MAX_INODES = 1024` |
| `total_blocks` | Total blocks on device |
| `inode_bmap_start` | Block index of inode bitmap start |
| `block_bmap_start` | Block index of block bitmap start |
| `inode_table_start` | Block index of inode table start |
| `data_start` | Block index of first data block |
| `root_inode` | Inode number of root directory (always 0) |
| `pad[484]` | Padding to fill exactly 512 bytes |

---

## `sfs_inode_t` (32 bytes)

```c
typedef struct sfs_inode {
    uint16_t type;                      // SFS_TYPE_FREE, FILE, or DIR
    uint16_t mode;                      // permission bits (unused)
    uint32_t size;                      // file size in bytes
    uint32_t direct[12];                // direct block pointers (12 × 512 = 6 KB max)
    uint32_t indirect;                  // singly-indirect block pointer
    uint8_t  pad[8];
} sfs_inode_t;
```

Maximum file size: 12 direct + 128 indirect (512/4) = 140 blocks × 512 B
= **71,680 bytes** (~70 KB).

---

## `sfs_dirent_t` (32 bytes)

```c
typedef struct sfs_dirent {
    uint32_t inode;     // inode number (0 = free/deleted slot)
    char     name[28];  // filename (null-terminated, max 27 chars)
} sfs_dirent_t;
```

Directory entries are stored sequentially in data blocks.  Deleted entries
have `inode = 0` and are skipped during lookup and `readdir`.

---

## Allocation: `sfs_bmap_alloc` / `sfs_bmap_free`

Generic bitmap allocator for both inode and block bitmaps.  Scans the
appropriate bitmap blocks for the first clear bit (0 = free), sets it,
and returns the index.  Uses 1 = allocated, 0 = free convention
(opposite of the PMM bitmap).

---

## Block addressing: `sfs_inode_get_block`

Maps a logical file block number to a physical block number:

- Block 0–11: direct → `inode->direct[file_block]`
- Block 12–139: indirect → reads `inode->indirect` block, uses it as an
  array of 128 `uint32_t` block pointers

If `create=1`, missing blocks are allocated on demand.  If `create=0`,
missing blocks return -1 (sparse file / beyond EOF).

---

## VFS operations

All VFS operations use an `sfs_file_t` stored in `vfs_node_t->private_data`:

```c
typedef struct sfs_file {
    sfs_fs_t*   fs;
    int         inum;     // inode number
    sfs_inode_t inode;    // cached inode
    uint32_t    offset;   // (unused — VFS tracks offset in fd_table)
} sfs_file_t;
```

### `sfs_vfs_read`
Translates byte range to block numbers, calls `sfs_inode_get_block` for
each block, reads 512-byte blocks, copies the requested sub-range.

### `sfs_vfs_write`
Same, but uses `sfs_inode_get_block(..., create=1)` to allocate blocks.
Updates `inode->size` and calls `sfs_write_inode` to persist.

### `sfs_vfs_readdir`
Computes the `index`-th directory entry, reads the containing block,
returns a heap-allocated `vfs_node_t` with `private_data = sfs_file_t*`.
The caller (`vfs.c` → `shell.c`) is responsible for freeing both.

### `sfs_vfs_create`
Allocates an inode, writes a zeroed `sfs_inode_t`, then adds a dirent
to the parent directory by appending to the directory's data via
`sfs_add_dirent`.

### `sfs_vfs_unlink`
Looks up the entry in the parent directory, frees all data blocks
(direct + indirect), frees the inode, removes the dirent (zeroes inode
field in the block).

### `sfs_vfs_truncate`
Frees blocks beyond `size`, updates `inode->size`.

---

## `sfs_format`

Writes the superblock, zeros all metadata blocks, marks inode 0 (root)
as allocated in the inode bitmap, writes an empty root directory inode
(`type=SFS_TYPE_DIR, size=0`).

---

## `sfs_mount`

Reads and validates the superblock (`magic == SFS_MAGIC`).  Allocates
`sfs_fs_t` via `kmalloc`, creates an `sfs_file_t` for the root inode,
fills `fs->root_node`, calls `vfs_register_fs`.

---

## Key design choices

- **No journaling / no crash recovery:** All writes go directly to the
  block device.  Power loss mid-write leaves the filesystem in an
  inconsistent state.
- **No block cache:** Every read/write goes through `block_read`/`block_write`
  (which for `ramdisk_blk` is just a `kmemcpy`).
- **Fixed layout parameters:** `SFS_MAX_INODES = 1024`, `SFS_BLOCK_SIZE = 512`.
  Changing these requires reformatting.
