#ifndef TMPFS_H
#define TMPFS_H

#include "types.h"
#include "vfs.h"

#define TMPFS_NAME_MAX 60

typedef struct tmpfs_dirent {
    char name[TMPFS_NAME_MAX];
    struct tmpfs_file* file;
    struct tmpfs_dirent* next;
} tmpfs_dirent_t;

typedef struct tmpfs_file {
    int type;
    uint32_t size;
    uint32_t nblocks;
    uintptr_t* blocks;
    tmpfs_dirent_t* entries;
    int nlink;
} tmpfs_file_t;

typedef struct tmpfs_fs {
    vfs_fs_t vfs_fs;
    vfs_node_t root_node;
    tmpfs_file_t* root_dir;
} tmpfs_fs_t;

err_t tmpfs_mount(vfs_fs_t** out_fs);

#endif