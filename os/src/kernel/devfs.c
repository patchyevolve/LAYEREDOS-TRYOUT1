#include "kernel.h"
#include "devfs.h"
#include "vfs.h"
#include "kmalloc.h"
#include "tty.h"

enum dev_type {
    DEV_NULL,
    DEV_ZERO,
    DEV_RANDOM,
    DEV_FULL,
    DEV_TTY,
};

typedef struct devfs_file {
    int type;
    uint32_t size;
} devfs_file_t;

#define DEVFS_ENTRIES 5

static devfs_file_t dev_null   = { DEV_NULL,   0 };
static devfs_file_t dev_zero   = { DEV_ZERO,   0 };
static devfs_file_t dev_random = { DEV_RANDOM, 0 };
static devfs_file_t dev_full   = { DEV_FULL,   0 };
static devfs_file_t dev_tty    = { DEV_TTY,   0 };

static const char* dev_names[DEVFS_ENTRIES] = {
    "null", "zero", "random", "full", "ttyS0"
};
static devfs_file_t* dev_files[DEVFS_ENTRIES] = {
    &dev_null, &dev_zero, &dev_random, &dev_full, &dev_tty
};

static int devfs_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int devfs_vfs_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int64_t devfs_vfs_read_null(devfs_file_t* f, void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)buf; (void)count; (void)offset;
    return 0;
}

static int64_t devfs_vfs_read_zero(devfs_file_t* f, void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)offset;
    kmemset(buf, 0, (size_t)count);
    return (int64_t)count;
}

static int64_t devfs_vfs_read_random(devfs_file_t* f, void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)offset;
    static uint32_t seed = 0xDEADBEEF;
    uint8_t* b = (uint8_t*)buf;
    for (uint64_t i = 0; i < count; i++) {
        seed = seed * 1103515245 + 12345;
        b[i] = (uint8_t)(seed >> 16);
    }
    return (int64_t)count;
}

static int64_t devfs_vfs_read_full(devfs_file_t* f, void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)offset;
    kmemset(buf, 0, (size_t)count);
    return (int64_t)count;
}

static int64_t devfs_vfs_write_null(devfs_file_t* f, const void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)buf; (void)offset;
    return (int64_t)count;
}

static int64_t devfs_vfs_write_full(devfs_file_t* f, const void* buf, uint64_t count, uint64_t offset) {
    (void)f; (void)buf; (void)offset;
    return -1;
}

static int64_t devfs_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    devfs_file_t* f = (devfs_file_t*)node->private_data;
    if (!f) return -1;
    switch (f->type) {
        case DEV_NULL:   return devfs_vfs_read_null(f, buf, count, offset);
        case DEV_ZERO:   return devfs_vfs_read_zero(f, buf, count, offset);
        case DEV_RANDOM: return devfs_vfs_read_random(f, buf, count, offset);
        case DEV_FULL:   return devfs_vfs_read_full(f, buf, count, offset);
        case DEV_TTY:    return tty_vfs_read(&tty_console.node, buf, count, offset);
        default: return -1;
    }
}

static int64_t devfs_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    devfs_file_t* f = (devfs_file_t*)node->private_data;
    if (!f) return -1;
    switch (f->type) {
        case DEV_NULL:   return devfs_vfs_write_null(f, buf, count, offset);
        case DEV_ZERO:   return devfs_vfs_write_null(f, buf, count, offset);
        case DEV_RANDOM: return devfs_vfs_write_null(f, buf, count, offset);
        case DEV_FULL:   return devfs_vfs_write_full(f, buf, count, offset);
        case DEV_TTY:    return tty_vfs_write(&tty_console.node, buf, count, offset);
        default: return -1;
    }
}

static int devfs_vfs_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    (void)node;
    if (index >= (uint32_t)DEVFS_ENTRIES) return -1;
    vfs_node_t* child = kmalloc(sizeof(vfs_node_t));
    if (!child) return -1;
    kmemset(child, 0, sizeof(vfs_node_t));
    kstrncpy(child->name, dev_names[index], VFS_MAX_NAME - 1);
    child->flags = 0;
    child->size = 0;
    child->fs = node->fs;
    child->private_data = dev_files[index];
    *out = child;
    return 0;
}

static int devfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir) {
    (void)dir; (void)name; (void)is_dir;
    return -1;
}

static int devfs_vfs_unlink(vfs_node_t* dir, const char* name) {
    (void)dir; (void)name;
    return -1;
}

static int devfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    devfs_file_t* f = (devfs_file_t*)node->private_data;
    if (!f) return -1;
    st->size = 0;
    st->inode = (uint64_t)(uintptr_t)f;
    st->mode = 0666;
    st->flags = node->flags;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    st->fs_flags = 0;
    return 0;
}

static int devfs_vfs_truncate(vfs_node_t* node, uint64_t size) {
    (void)node; (void)size;
    return 0;
}

static int devfs_vfs_chmod(vfs_node_t* node, uint32_t mode) {
    (void)node; (void)mode;
    return 0;
}

static int devfs_vfs_lock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int devfs_vfs_unlock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int devfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                            vfs_node_t* new_dir, const char* new_name) {
    (void)old_dir; (void)old_name; (void)new_dir; (void)new_name;
    return -1;
}

static int devfs_vfs_link(vfs_node_t* dir, const char* name, vfs_node_t* target) {
    (void)dir; (void)name; (void)target;
    return -1;
}

static vfs_file_ops_t devfs_ops = {
    .open     = devfs_vfs_open,
    .close    = devfs_vfs_close,
    .read     = devfs_vfs_read,
    .write    = devfs_vfs_write,
    .readdir  = devfs_vfs_readdir,
    .create   = devfs_vfs_create,
    .unlink   = devfs_vfs_unlink,
    .rename   = devfs_vfs_rename,
    .link     = devfs_vfs_link,
    .stat     = devfs_vfs_stat,
    .truncate = devfs_vfs_truncate,
    .chmod    = devfs_vfs_chmod,
    .lock     = devfs_vfs_lock,
    .unlock   = devfs_vfs_unlock,
};

typedef struct devfs_fs {
    vfs_fs_t vfs_fs;
    vfs_node_t root_node;
} devfs_fs_t;

static devfs_fs_t* devfs_fs = NULL;

err_t devfs_mount(void) {
    devfs_fs = kmalloc(sizeof(devfs_fs_t));
    if (!devfs_fs) return ERR_NOMEM;
    kmemset(devfs_fs, 0, sizeof(devfs_fs_t));

    kstrncpy(devfs_fs->vfs_fs.name, "devfs", sizeof(devfs_fs->vfs_fs.name) - 1);
    devfs_fs->vfs_fs.root = &devfs_fs->root_node;
    devfs_fs->vfs_fs.ops = &devfs_ops;

    kmemset(&devfs_fs->root_node, 0, sizeof(devfs_fs->root_node));
    kstrncpy(devfs_fs->root_node.name, "/", VFS_MAX_NAME - 1);
    devfs_fs->root_node.flags = 1;
    devfs_fs->root_node.size = 0;
    devfs_fs->root_node.fs = &devfs_fs->vfs_fs;
    devfs_fs->root_node.private_data = NULL;

    err_t e = vfs_mount("/dev", &devfs_fs->vfs_fs);
    if (e) { kfree(devfs_fs); devfs_fs = NULL; return e; }

    kprintf("[DEVFS] Mounted at /dev\n");
    return ERR_OK;
}
