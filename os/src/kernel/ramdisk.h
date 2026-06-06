#ifndef RAMDISK_H
#define RAMDISK_H

#include "types.h"
#include "vfs.h"

#define RAMDISK_MAX_FILES 64
#define RAMDISK_NAME_MAX  64

typedef struct {
    char     name[RAMDISK_NAME_MAX];
    uint8_t* data;
    uint64_t size;
} ramdisk_file_t;

typedef struct {
    ramdisk_file_t files[RAMDISK_MAX_FILES];
    int           file_count;
    vfs_node_t    root;
    vfs_fs_t      fs;
} ramdisk_t;

err_t ramdisk_init(void);
int   ramdisk_add_file(const char* name, const void* data, uint64_t size);

#endif
