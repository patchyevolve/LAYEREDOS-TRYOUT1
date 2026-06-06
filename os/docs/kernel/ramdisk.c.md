# ramdisk.c — VFS-backed In-Memory Filesystem (Read-only)

**Path:** `os/src/kernel/ramdisk.c`  
**Layer:** Layer 5 (Filesystem)

---

## Purpose

Provides a simple read-only in-memory filesystem accessible via the VFS.
Files are registered at boot time by calling `ramdisk_add_file(name, data,
size)` with pointers to static kernel data (e.g., the embedded user binary,
a version string).  The shell's `ls` and `cat` commands can then read them.

This is distinct from `ramdisk_blk` — that is a writable block device
(raw sector storage).  This `ramdisk` is a VFS filesystem backed by static
kernel-space memory pointers, not a block device.

---

## `ramdisk_t` structure

```c
typedef struct {
    ramdisk_file_t files[64];   // file metadata array
    int           file_count;
    vfs_node_t    root;         // "/" directory VFS node
    vfs_fs_t      fs;           // filesystem descriptor
} ramdisk_t;
```

One global `ramdisk` instance.  All VFS nodes for files are allocated
dynamically from PMM pages and linked as children of `root`.

---

## `ramdisk_init`

1. Zeroes the `ramdisk` struct.
2. Names the root node `"/"`, sets `flags=1` (directory).
3. Names the filesystem `"ramdisk"`, links `root` as `fs.root`.
4. Calls `vfs_register_fs(&ramdisk.fs)` — mounts it as the active filesystem.

---

## `ramdisk_add_file(name, data, size)`

1. Fills a `ramdisk_file_t` slot with name, data pointer, and size.
   The `data` pointer is **not copied** — it points directly to the source
   (e.g., into `_binary_build_user_program_bin_start`).
2. Allocates a `vfs_node_t` page from PMM.
3. Sets `node->private_data = &files[n]`.
4. Appends the node to `root.children` list.

**Memory note:** VFS nodes are leaked (never freed) since the ramdisk is
boot-time only.  Files themselves are not copied — reads directly access
the original data pointer.

---

## VFS operations

| Op | Behaviour |
|----|-----------|
| `open` | No-op (always succeeds) |
| `close` | No-op |
| `read` | `kmemcpy(buf, f->data + offset, count)` with bounds check |
| `write` | Returns -1 (read-only) |
| `readdir` | Traverses `node->children` linked list by index |

`readdir` is used by the shell's `ls` command (`cmd_ls`): it repeatedly
calls `readdir(root, idx, &child)` incrementing `idx` until it returns -1.
