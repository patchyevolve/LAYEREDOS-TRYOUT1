# OPERtur/TRY1 — Kernel Project Summary

**Last updated:** June 6, 2026

## Project Overview

OPERtur/TRY1 is an OS kernel being developed from scratch. The codebase is in `os/` and the architecture is x86-64.

## Current Session Accomplishments (June 6, 2026)

1. **Fixed `sfs_add_dirent` to write to correct parent directory** (`os/src/kernel/sfs.c`): Previously always wrote to `fs->vfs_fs.root` (the root directory). Changed to create a temporary `vfs_node_t` wrapper around the actual parent directory so subdirectory operations work correctly.

2. **Fixed stale inode cache in `sfs_vfs_readdir`** (`os/src/kernel/sfs.c`): Added `sfs_read_inode(f->fs, f->inum, &f->inode)` at the start of `readdir` to refresh parent directory inode data from disk. Without this, cached `private_data->inode.size` became stale after directory modifications (e.g., creating a file in a subdirectory), causing `vfs_find` to fail when walking paths into subdirectories.

3. **Production-ready `cmd_ls`** (`os/src/kernel/shell.c`): Uses `readdir` (SFS-compatible) with fallback to children linked list. Root node is now persistent and not freed.

4. **PMM virtual address fix** (`os/src/kernel/pmm.c`): Changed PMM free list to use kernel virtual addresses (via `PHYS_TO_VIRT`/`VIRT_TO_PHYS`) instead of direct physical addresses. This fixes page faults in syscall handlers that run with user CR3 (which lacks identity mapping in the low half).

5. **Verified all operations work**: `mkdir`, `writefile`, `cat`, `ls`, `rm`, `rmdir` all work correctly across nested directory structures.

## Status

The kernel supports a basic shell with file system operations on a simple SFS (Simple File System). The PMM now correctly handles virtual vs physical address translation, and the VFS layer properly supports nested directories.
