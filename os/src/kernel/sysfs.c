#include "kernel.h"
#include "sysfs.h"
#include "vfs.h"
#include "kmalloc.h"
#include "block.h"
#include "hal.h"
#include "smp.h"

static int sfmt(char* buf, int sz, const char* f, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = kvsnprintf(buf, (size_t)sz, f, ap);
    __builtin_va_end(ap);
    return n;
}

enum sysfs_type {
    SYSFS_ROOT,
    SYSFS_BLOCK,
    SYSFS_BLOCK_DEV_DIR,
    SYSFS_BLOCK_DEV_SIZE,
    SYSFS_BLOCK_DEV_SECTOR_SIZE,
    SYSFS_KERNEL,
    SYSFS_VERSION,
    SYSFS_UPTIME,
};

typedef struct {
    int type;
    int blk_idx;
} sysfs_info_t;

/* Root directory entries */
#define SYSFS_ROOT_ENTRIES 2
static const char* sysfs_root_names[SYSFS_ROOT_ENTRIES] = {
    "block", "kernel"
};
static const int sysfs_root_types[SYSFS_ROOT_ENTRIES] = {
    SYSFS_BLOCK, SYSFS_KERNEL
};

/* Block device subdirectory file entries */
#define SYSFS_BLOCK_DEV_FILES 2
static const char* sysfs_block_dev_fnames[SYSFS_BLOCK_DEV_FILES] = {
    "size", "sector_size"
};
static const int sysfs_block_dev_ftypes[SYSFS_BLOCK_DEV_FILES] = {
    SYSFS_BLOCK_DEV_SIZE, SYSFS_BLOCK_DEV_SECTOR_SIZE
};

/* Kernel subdirectory entries */
#define SYSFS_KERNEL_ENTRIES 2
static const char* sysfs_kernel_names[SYSFS_KERNEL_ENTRIES] = {
    "version", "uptime"
};
static const int sysfs_kernel_types[SYSFS_KERNEL_ENTRIES] = {
    SYSFS_VERSION, SYSFS_UPTIME
};

static vfs_node_t* sysfs_alloc_child(vfs_fs_t* fs, const char* name,
                                     int type, int blk_idx) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    kmemset(node, 0, sizeof(*node));
    kstrncpy(node->name, name, VFS_MAX_NAME - 1);
    node->name[VFS_MAX_NAME - 1] = 0;
    node->flags = (type == SYSFS_BLOCK || type == SYSFS_BLOCK_DEV_DIR ||
                   type == SYSFS_KERNEL) ? 1 : 0;
    node->fs = fs;
    /* Children are cached in dentry tree — no dynamic freeing */
    sysfs_info_t* info = (sysfs_info_t*)kmalloc(sizeof(sysfs_info_t));
    if (!info) { kfree(node); return NULL; }
    info->type = type;
    info->blk_idx = blk_idx;
    node->private_data = info;
    return node;
}

static int sysfs_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int sysfs_vfs_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int write_uptime(char* buf, int sz) {
    uint64_t ns = hal_timer_get_ns();
    uint64_t secs = ns / 1000000000ULL;
    uint64_t frac = (ns % 1000000000ULL) / 10000000ULL; /* hundredths */
    return sfmt(buf, sz, "%lu.%02lu\n", (unsigned long)secs, (unsigned long)frac);
}

static int write_version(char* buf, int sz) {
    return sfmt(buf, sz, "OPERtur/TRY1 OS v0.2.0 (%d CPU)\n", smp_nr_cpus());
}

static int write_block_size(char* buf, int sz, int blk_idx) {
    block_dev_t* dev = block_get(blk_idx);
    if (!dev) return sfmt(buf, sz, "0\n");
    return sfmt(buf, sz, "%lu\n", (unsigned long)(dev->block_count * dev->block_size));
}

static int write_block_sector_size(char* buf, int sz, int blk_idx) {
    block_dev_t* dev = block_get(blk_idx);
    if (!dev) return sfmt(buf, sz, "0\n");
    return sfmt(buf, sz, "%u\n", dev->block_size);
}

static int64_t sysfs_vfs_read(vfs_node_t* node, void* buf,
                              uint64_t count, uint64_t offset) {
    if (!node || !node->private_data || !buf) return -1;
    sysfs_info_t* info = (sysfs_info_t*)node->private_data;
    char tmp[256];
    int len = 0;

    switch (info->type) {
    case SYSFS_VERSION:
        len = write_version(tmp, sizeof(tmp));
        break;
    case SYSFS_UPTIME:
        len = write_uptime(tmp, sizeof(tmp));
        break;
    case SYSFS_BLOCK_DEV_SIZE:
        len = write_block_size(tmp, sizeof(tmp), info->blk_idx);
        break;
    case SYSFS_BLOCK_DEV_SECTOR_SIZE:
        len = write_block_sector_size(tmp, sizeof(tmp), info->blk_idx);
        break;
    default:
        return 0;
    }

    if (len < 0) return -1;
    if ((int64_t)offset >= len) return 0;
    uint64_t avail = (uint64_t)len - offset;
    if (count > avail) count = avail;
    kmemcpy(buf, tmp + offset, count);
    return (int64_t)count;
}

static int sysfs_vfs_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    if (!node || !node->private_data || !out) return -1;
    sysfs_info_t* info = (sysfs_info_t*)node->private_data;
    *out = NULL;

    switch (info->type) {
    case SYSFS_ROOT:
        if (index < (uint32_t)SYSFS_ROOT_ENTRIES) {
            *out = sysfs_alloc_child(node->fs, sysfs_root_names[index],
                                     sysfs_root_types[index], 0);
            return *out ? 0 : -1;
        }
        return -1;

    case SYSFS_BLOCK: {
        int blk_idx = (int)index;
        block_dev_t* dev = block_get(blk_idx);
        if (!dev) return -1;
        *out = sysfs_alloc_child(node->fs, dev->name,
                                 SYSFS_BLOCK_DEV_DIR, blk_idx);
        return *out ? 0 : -1;
    }

    case SYSFS_BLOCK_DEV_DIR:
        if (index < (uint32_t)SYSFS_BLOCK_DEV_FILES) {
            *out = sysfs_alloc_child(node->fs, sysfs_block_dev_fnames[index],
                                     sysfs_block_dev_ftypes[index], info->blk_idx);
            return *out ? 0 : -1;
        }
        return -1;

    case SYSFS_KERNEL:
        if (index < (uint32_t)SYSFS_KERNEL_ENTRIES) {
            *out = sysfs_alloc_child(node->fs, sysfs_kernel_names[index],
                                     sysfs_kernel_types[index], 0);
            return *out ? 0 : -1;
        }
        return -1;

    default:
        return -1;
    }
}

static int sysfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !st) return -1;
    kmemset(st, 0, sizeof(*st));
    st->size = 0;
    st->mode = (node->flags & 1) ? 0555 : 0444;
    st->uid  = 0;
    st->gid  = 0;
    return 0;
}

static vfs_file_ops_t sysfs_ops = {
    .open    = sysfs_vfs_open,
    .close   = sysfs_vfs_close,
    .read    = sysfs_vfs_read,
    .readdir = sysfs_vfs_readdir,
    .stat    = sysfs_vfs_stat,
};

static vfs_fs_t* sysfs_fs = NULL;

err_t sysfs_mount(void) {
    vfs_fs_t* fs = (vfs_fs_t*)kmalloc(sizeof(vfs_fs_t));
    if (!fs) return ERR_NOMEM;
    kmemset(fs, 0, sizeof(*fs));
    kstrncpy(fs->name, "sysfs", sizeof(fs->name) - 1);
    fs->ops = &sysfs_ops;

    vfs_node_t* root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!root) { kfree(fs); return ERR_NOMEM; }
    kmemset(root, 0, sizeof(*root));
    kstrncpy(root->name, "/", VFS_MAX_NAME - 1);
    root->flags = 1;
    root->fs = fs;
    root->dynamic = 1;
    sysfs_info_t* root_info = (sysfs_info_t*)kmalloc(sizeof(sysfs_info_t));
    if (!root_info) { kfree(root); kfree(fs); return ERR_NOMEM; }
    root_info->type = SYSFS_ROOT;
    root_info->blk_idx = 0;
    root->private_data = root_info;
    fs->root = root;

    err_t e = vfs_mount("/sys", fs);
    if (e != ERR_OK) {
        kfree(root_info);
        kfree(root);
        kfree(fs);
        return e;
    }
    sysfs_fs = fs;
    kprintf("[SYSFS] mounted at /sys\n");
    return ERR_OK;
}
