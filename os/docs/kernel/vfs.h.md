# vfs.h — Virtual Filesystem Interface

**Path:** `os/src/kernel/vfs.h`  
**Layer:** Layer 5 (Filesystem) — header

---

## Purpose

Defines all VFS types (`vfs_node_t`, `vfs_fs_t`, `vfs_file_ops_t`,
`vfs_fd_t`) and declares the VFS API.  Included by every filesystem driver
(`ramdisk.c`, `sfs.c`, `pipe.c`) and by the shell (`shell.c`) which calls
VFS functions directly.

---

## Constants

```c
#define VFS_MAX_NAME  64    // max chars in a filename (including null)
#define VFS_MAX_FILES 64    // reserved (unused in current implementation)
#define VFS_MAX_FDS   32    // open file descriptors (global, not per-process)
#define VFS_SEEK_SET  0     // seek from start
#define VFS_SEEK_CUR  1     // seek from current position
#define VFS_SEEK_END  2     // seek from end
```

---

## `vfs_node_t`

```c
typedef struct vfs_node {
    char        name[64];       // filename component (not full path)
    uint32_t    flags;          // bit 0 = is_directory
    uint64_t    size;           // file size in bytes
    uint64_t    inode;          // inode number (filesystem-specific)
    vfs_node_t* parent;         // parent directory node
    vfs_node_t* children;       // first child (directory listing)
    vfs_node_t* next;           // next sibling in parent's children list
    vfs_fs_t*   fs;             // filesystem this node belongs to
    void*       private_data;   // filesystem-specific state (e.g., sfs_file_t*)
} vfs_node_t;
```

The `children`/`next` linked list is used by the static ramdisk.  SFS
uses `readdir` + `private_data` instead.

---

## `vfs_file_ops_t`

The vtable that every filesystem driver fills in:

| Function | Signature | Purpose |
|----------|-----------|---------|
| `open` | `(node) → int` | Called on `vfs_open`; 0=ok, -1=fail |
| `close` | `(node) → int` | Called on `vfs_close` |
| `read` | `(node, buf, count, offset) → int64_t` | Read bytes |
| `write` | `(node, buf, count, offset) → int64_t` | Write bytes |
| `readdir` | `(node, index, **out) → int` | Return child at index; 0=ok, -1=end |
| `create` | `(dir, name, is_dir) → int` | Create file/dir in `dir` |
| `unlink` | `(dir, name) → int` | Remove file/dir from `dir` |
| `stat` | `(node, *st) → int` | Fill `vfs_stat_t` |
| `truncate` | `(node, size) → int` | Resize file |

NULL entries in the vtable are handled gracefully by the VFS (they simply
return -1 / `ERR_INVAL`).

---

## `vfs_fs_t`

```c
typedef struct vfs_fs {
    char            name[16];   // "ramdisk", "sfs", "pipe"
    vfs_node_t*     root;       // root node of this filesystem
    vfs_file_ops_t* ops;        // vtable pointer
} vfs_fs_t;
```

---

## `vfs_fd_t`

```c
typedef struct vfs_fd {
    vfs_node_t* node;      // open file node
    uint64_t    offset;    // current read/write position
    int         flags;     // 0=read, 1=write
    int         used;      // 0=free slot, 1=open
} vfs_fd_t;
```

The global `fd_table[VFS_MAX_FDS]` is `extern`-declared here so `pipe.c`
can access it directly.

---

## API summary

| Function | Description |
|----------|-------------|
| `vfs_init()` | Zero fd table, init root node, reserve fds 0–2 |
| `vfs_register_fs(fs)` | Set as active filesystem, link root |
| `vfs_mount(path, fs)` | Same as register (path ignored currently) |
| `vfs_find(path)` | Walk path, return node or NULL |
| `vfs_open(path, flags)` | Find + allocate fd |
| `vfs_close(fd)` | Free fd slot |
| `vfs_read(fd, buf, count)` | Read from fd offset |
| `vfs_write(fd, buf, count)` | Write to fd offset |
| `vfs_lseek(fd, offset, whence)` | Seek fd |
| `vfs_create(path, is_dir)` | Create file or directory |
| `vfs_mkdir(path)` | Create directory |
| `vfs_unlink(path)` | Remove file |
| `vfs_rmdir(path)` | Remove directory |
| `vfs_stat(path, st)` | Get file metadata |
| `vfs_ftruncate(fd, size)` | Resize open file |
