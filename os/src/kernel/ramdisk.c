#include "kernel.h"
#include "ramdisk.h"
#include "vfs.h"
#include "pmm.h"

static ramdisk_t ramdisk;

static int ramdisk_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int ramdisk_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int64_t ramdisk_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    ramdisk_file_t* f = (ramdisk_file_t*)node->private_data;
    if (!f) return -1;
    if (offset >= f->size) return 0;
    if (offset + count > f->size)
        count = f->size - offset;
    kmemcpy(buf, f->data + offset, count);
    return (int64_t)count;
}

static int64_t ramdisk_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)node;
    (void)buf;
    (void)count;
    (void)offset;
    return -1;
}

static int ramdisk_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    if (!out) return -1;
    *out = NULL;

    vfs_node_t* child = node->children;
    uint32_t cnt = 0;
    while (child) {
        if (cnt == index) { *out = child; return 0; }
        cnt++;
        child = child->next;
    }
    return -1;
}

static vfs_file_ops_t ramdisk_ops = {
    .open    = ramdisk_open,
    .close   = ramdisk_close,
    .read    = ramdisk_read,
    .write   = ramdisk_write,
    .readdir = ramdisk_readdir,
    .create  = NULL,
    .unlink  = NULL,
};

err_t ramdisk_init(void) {
    kmemset(&ramdisk, 0, sizeof(ramdisk));

    kstrncpy(ramdisk.root.name, "/", VFS_MAX_NAME - 1);
    ramdisk.root.flags = 1;
    ramdisk.root.fs = &ramdisk.fs;

    kstrncpy(ramdisk.fs.name, "ramdisk", 7);
    ramdisk.fs.root = &ramdisk.root;
    ramdisk.fs.ops = &ramdisk_ops;

    vfs_register_fs(&ramdisk.fs);

    kprintf("[RAMDISK] Initialized\n");
    return ERR_OK;
}

int ramdisk_add_file(const char* name, const void* data, uint64_t size) {
    if (ramdisk.file_count >= RAMDISK_MAX_FILES)
        return -1;

    ramdisk_file_t* f = &ramdisk.files[ramdisk.file_count];
    kstrncpy(f->name, name, RAMDISK_NAME_MAX - 1);
    f->data = (uint8_t*)data;
    f->size = size;

    uint64_t node_phys = pmm_alloc_page();
    if (!node_phys) return -1;
    vfs_node_t* node = (vfs_node_t*)PHYS_TO_VIRT(node_phys);
    kmemset(node, 0, sizeof(vfs_node_t));
    kstrncpy(node->name, name, VFS_MAX_NAME - 1);
    node->size = size;
    node->private_data = f;
    node->fs = &ramdisk.fs;

    vfs_node_t* parent = vfs_find("/");
    if (parent) {
        node->next = parent->children;
        parent->children = node;
        node->parent = parent;
    }

    ramdisk.file_count++;
    return 0;
}
