# vfs.c — Virtual Filesystem Switch

**Path:** `os/src/kernel/vfs.c`  
**Layer:** Layer 5 (Filesystem)

---

## Purpose

Implements a minimal VFS layer that sits between the shell/syscalls and
the actual filesystem drivers (ramdisk, SFS).  Manages an open-file
descriptor table and dispatches read/write/create/unlink through the
`vfs_file_ops_t` function pointers of whichever filesystem is currently
mounted.

---

## Global state

```c
vfs_fd_t   fd_table[VFS_MAX_FDS];   // 32 open file descriptors
static vfs_node_t root_node;        // "/" — synthetic root
static vfs_fs_t*  mounted_fs;       // single mounted filesystem
```

Only one filesystem can be mounted at a time (`mounted_fs`).  `vfs_register_fs`
and `vfs_mount` both replace this pointer, so calling `ramdisk_init` and
then `sfs_mount` leaves SFS as the active filesystem.

**Slots 0–2 of `fd_table`** are marked `used=1` at init to reserve stdin
(0), stdout (1), and stderr (2), matching POSIX convention.

---

## `vfs_register_fs`

Sets `mounted_fs = fs`, points `root_node.fs = fs`, and hooks the
filesystem's own root node as a child of `root_node`.  This is the
lightweight alternative to `vfs_mount` used by `ramdisk_init`.

---

## `vfs_find(path)`

Path walker.  Two modes:

**Mode 1 — `readdir`-based (SFS):**  
If the mounted filesystem provides `ops->readdir`, `vfs_find` tokenises
the path by `'/'` and at each component calls `readdir(cur, idx, &child)`
in a loop until a matching `child->name` is found.  Handles paths like
`"/dir/file"` by walking component by component.

**Mode 2 — `children` list (ramdisk):**  
Falls back to the singly-linked `vfs_node_t->children`/`->next` list for
filesystems that pre-build their node tree (the static ramdisk).

---

## `vfs_open(path, flags)`

1. `vfs_find(path)` — get the node.
2. Find a free `fd_table` slot.
3. Call `node->fs->ops->open(node)` if defined.
4. Fill `fd_table[fd]` with node, offset=0, flags.
5. Return fd.

`flags = 0` = read, `flags = 1` = write (simple).  There is no `O_CREAT`
via `vfs_open`; callers must use `vfs_create` + `vfs_open` for new files.

---

## `vfs_read` / `vfs_write`

Both check fd validity, call the filesystem's `ops->read`/`ops->write` with
the current fd offset, then advance the offset by the number of bytes
transferred.  The offset is tracked per-fd in `fd_table[fd].offset`.

---

## `vfs_lseek`

Implements `SEEK_SET`, `SEEK_CUR`, `SEEK_END` by updating `fd_table[fd].offset`.
`SEEK_END` uses `node->size` as the base.

---

## `vfs_create` / `vfs_mkdir` / `vfs_unlink`

All three parse the path to extract a parent directory path and a filename,
find the parent node via `vfs_find`, then call `ops->create` / `ops->unlink`
on the parent with the filename.

`vfs_mkdir` calls `vfs_create(path, is_dir=1)`.  
`vfs_rmdir` verifies the node has `flags & 1` (is a directory), then
delegates to `vfs_unlink`.

---

## `vfs_stat`

Returns file metadata into a `vfs_stat_t`.  If the filesystem provides
`ops->stat`, delegates to it (SFS does this).  Otherwise synthesises the
struct from `node->size`, `node->inode`, `node->flags`.

---

## Known limitations

- Single mounted filesystem — no mount namespaces, no `umount`.
- No path canonicalisation — `".."` and `"."` are not handled.
- No permission checking.
- `fd_table` is global — shared across all processes (no per-process FD
  table yet).
- Linear `vfs_find` scan per path component — O(entries) for each level.
