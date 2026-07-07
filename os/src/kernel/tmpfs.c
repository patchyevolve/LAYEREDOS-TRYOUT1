#include "kernel.h"
#include "tmpfs.h"
#include "pmm.h"
#include "kmalloc.h"

#define TMPFS_TYPE_FILE 1
#define TMPFS_TYPE_DIR  2

static tmpfs_file_t* tmpfs_create_file(int type) {
    tmpfs_file_t* f = kmalloc(sizeof(tmpfs_file_t));
    if (!f) return NULL;
    kmemset(f, 0, sizeof(tmpfs_file_t));
    f->type = type;
    f->nlink = 1;
    if (type == TMPFS_TYPE_DIR)
        f->entries = NULL;
    return f;
}

static tmpfs_dirent_t* tmpfs_lookup(tmpfs_file_t* dir, const char* name) {
    if (!dir || dir->type != TMPFS_TYPE_DIR) return NULL;
    tmpfs_dirent_t* d = dir->entries;
    while (d) {
        if (kstrcmp(d->name, name) == 0) return d;
        d = d->next;
    }
    return NULL;
}

static int tmpfs_add_dirent(tmpfs_file_t* dir, const char* name, tmpfs_file_t* file) {
    if (!dir || !name || !file) return -1;
    if (tmpfs_lookup(dir, name)) return -1;
    tmpfs_dirent_t* d = kmalloc(sizeof(tmpfs_dirent_t));
    if (!d) return -1;
    kmemset(d, 0, sizeof(tmpfs_dirent_t));
    kstrncpy(d->name, name, TMPFS_NAME_MAX - 1);
    d->file = file;
    d->next = dir->entries;
    dir->entries = d;
    return 0;
}

static void tmpfs_remove_dirent(tmpfs_file_t* dir, const char* name) {
    if (!dir || dir->type != TMPFS_TYPE_DIR) return;
    tmpfs_dirent_t** pp = &dir->entries;
    while (*pp) {
        tmpfs_dirent_t* d = *pp;
        if (kstrcmp(d->name, name) == 0) {
            *pp = d->next;
            kfree(d);
            return;
        }
        pp = &d->next;
    }
}

static int tmpfs_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int tmpfs_vfs_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static tmpfs_fs_t* tmpfs_from_node(vfs_node_t* node) {
    return (tmpfs_fs_t*)node->fs;
}


static int64_t tmpfs_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    tmpfs_file_t* f = (tmpfs_file_t*)node->private_data;
    if (!f || f->type != TMPFS_TYPE_FILE) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(node)->lock, &_sflags);
    if (offset >= f->size) { spinlock_release(&tmpfs_from_node(node)->lock, _sflags); return 0; }
    if (offset + count > f->size)
        count = f->size - offset;
    uint64_t done = 0;
    while (done < count) {
        uint32_t bi = (uint32_t)((offset + done) / PAGE_SIZE);
        uint32_t off = (uint32_t)((offset + done) % PAGE_SIZE);
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > count - done) chunk = (uint32_t)(count - done);
        if (bi >= f->nblocks || !f->blocks[bi]) break;
        uint8_t* src = (uint8_t*)PHYS_TO_VIRT(f->blocks[bi]) + off;
        kmemcpy((uint8_t*)buf + done, src, chunk);
        done += chunk;
    }
    spinlock_release(&tmpfs_from_node(node)->lock, _sflags);
    return (int64_t)done;
}

static int64_t tmpfs_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    tmpfs_file_t* f = (tmpfs_file_t*)node->private_data;
    if (!f || f->type != TMPFS_TYPE_FILE) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(node)->lock, &_sflags);

    uint64_t end = offset + count;
    uint32_t need_blocks = (uint32_t)((end + PAGE_SIZE - 1) / PAGE_SIZE);
    if (need_blocks > f->nblocks) {
        uintptr_t* new_blocks = kmalloc((size_t)need_blocks * sizeof(uintptr_t));
        if (!new_blocks) { spinlock_release(&tmpfs_from_node(node)->lock, _sflags); return -1; }
        kmemcpy(new_blocks, f->blocks, (size_t)f->nblocks * sizeof(uintptr_t));
        kmemset(new_blocks + f->nblocks, 0, (size_t)(need_blocks - f->nblocks) * sizeof(uintptr_t));
        if (f->blocks) kfree(f->blocks);
        f->blocks = new_blocks;
        f->nblocks = need_blocks;
    }

    uint64_t done = 0;
    while (done < count) {
        uint32_t bi = (uint32_t)((offset + done) / PAGE_SIZE);
        uint32_t off = (uint32_t)((offset + done) % PAGE_SIZE);
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        if (!f->blocks[bi]) {
            uint64_t page = pmm_alloc_page();
            if (!page) break;
            f->blocks[bi] = page;
        }
        uint8_t* dst = (uint8_t*)PHYS_TO_VIRT(f->blocks[bi]) + off;
        kmemcpy(dst, (const uint8_t*)buf + done, chunk);
        done += chunk;
    }

    if (end > f->size)
        f->size = (uint32_t)end;
    node->size = f->size;
    spinlock_release(&tmpfs_from_node(node)->lock, _sflags);
    return (int64_t)done;
}

static int tmpfs_vfs_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    tmpfs_file_t* f = (tmpfs_file_t*)node->private_data;
    if (!f || f->type != TMPFS_TYPE_DIR) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(node)->lock, &_sflags);

    tmpfs_dirent_t* d = f->entries;
    uint32_t i = 0;
    while (d && i < index) { d = d->next; i++; }
    if (!d) { spinlock_release(&tmpfs_from_node(node)->lock, _sflags); return -1; }

    vfs_node_t* child = kmalloc(sizeof(vfs_node_t));
    if (!child) { spinlock_release(&tmpfs_from_node(node)->lock, _sflags); return -1; }
    kmemset(child, 0, sizeof(vfs_node_t));
    kstrncpy(child->name, d->name, VFS_MAX_NAME - 1);
    child->size = d->file->size;
    child->flags = (d->file->type == TMPFS_TYPE_DIR) ? 1 : 0;
    child->fs = node->fs;
    child->private_data = d->file;
    *out = child;
    spinlock_release(&tmpfs_from_node(node)->lock, _sflags);
    return 0;
}

static int tmpfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir) {
    tmpfs_file_t* parent = (tmpfs_file_t*)dir->private_data;
    if (!parent) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(dir)->lock, &_sflags);

    if (tmpfs_lookup(parent, name)) { spinlock_release(&tmpfs_from_node(dir)->lock, _sflags); return -1; }

    tmpfs_file_t* f = tmpfs_create_file(is_dir ? TMPFS_TYPE_DIR : TMPFS_TYPE_FILE);
    if (!f) { spinlock_release(&tmpfs_from_node(dir)->lock, _sflags); return -1; }

    if (tmpfs_add_dirent(parent, name, f) < 0) {
        kfree(f);
        spinlock_release(&tmpfs_from_node(dir)->lock, _sflags);
        return -1;
    }
    spinlock_release(&tmpfs_from_node(dir)->lock, _sflags);
    return 0;
}

static int tmpfs_vfs_unlink(vfs_node_t* dir, const char* name) {
    tmpfs_file_t* parent = (tmpfs_file_t*)dir->private_data;
    if (!parent) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(dir)->lock, &_sflags);

    tmpfs_dirent_t* d = tmpfs_lookup(parent, name);
    if (!d) { spinlock_release(&tmpfs_from_node(dir)->lock, _sflags); return -1; }

    tmpfs_file_t* f = d->file;
    if (f->nlink > 1) {
        f->nlink--;
    } else {
        for (uint32_t i = 0; i < f->nblocks; i++)
            if (f->blocks[i]) pmm_free_page(f->blocks[i]);
        if (f->blocks) kfree(f->blocks);
        kfree(f);
    }
    tmpfs_remove_dirent(parent, name);
    spinlock_release(&tmpfs_from_node(dir)->lock, _sflags);
    return 0;
}

static int tmpfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    tmpfs_file_t* f = (tmpfs_file_t*)node->private_data;
    if (!f) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(node)->lock, &_sflags);
    st->size  = f->size;
    st->inode = (uint64_t)(uintptr_t)f;
    st->mode  = (f->type == TMPFS_TYPE_DIR) ? (0644 | (1 << 16)) : 0644;
    st->flags = node->flags;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    st->fs_flags = 0;
    st->uid  = node->uid;
    st->gid  = node->gid;
    spinlock_release(&tmpfs_from_node(node)->lock, _sflags);
    return 0;
}

static int tmpfs_vfs_truncate(vfs_node_t* node, uint64_t size) {
    tmpfs_file_t* f = (tmpfs_file_t*)node->private_data;
    if (!f || f->type != TMPFS_TYPE_FILE) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(node)->lock, &_sflags);

    uint32_t new_blocks = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    while (f->nblocks > new_blocks) {
        f->nblocks--;
        if (f->blocks[f->nblocks]) pmm_free_page(f->blocks[f->nblocks]);
    }
    f->size = (uint32_t)size;
    node->size = f->size;
    spinlock_release(&tmpfs_from_node(node)->lock, _sflags);
    return 0;
}

static int tmpfs_vfs_chmod(vfs_node_t* node, uint32_t mode) {
    (void)node; (void)mode;
    return 0;
}

static int tmpfs_vfs_lock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int tmpfs_vfs_unlock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int tmpfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                            vfs_node_t* new_dir, const char* new_name) {
    tmpfs_file_t* old_parent = (tmpfs_file_t*)old_dir->private_data;
    tmpfs_file_t* new_parent = (tmpfs_file_t*)new_dir->private_data;
    if (!old_parent || !new_parent) return -1;

    /* Lock both filesystems — if same, just one lock */
    tmpfs_fs_t* old_fs = tmpfs_from_node(old_dir);
    tmpfs_fs_t* new_fs = tmpfs_from_node(new_dir);
    if (old_fs == new_fs) {
        cpu_flags_t _sflags;
        spinlock_acquire(&old_fs->lock, &_sflags);
        tmpfs_dirent_t* d = tmpfs_lookup(old_parent, old_name);
        if (!d) { spinlock_release(&old_fs->lock, _sflags); return -1; }
        tmpfs_dirent_t* t = tmpfs_lookup(new_parent, new_name);
        if (t) {
            if (t->file == d->file) { spinlock_release(&old_fs->lock, _sflags); return 0; }
            tmpfs_file_t* tf = t->file;
            if (tf->nlink > 1) { tf->nlink--; }
            else {
                for (uint32_t i = 0; i < tf->nblocks; i++)
                    if (tf->blocks[i]) pmm_free_page(tf->blocks[i]);
                if (tf->blocks) kfree(tf->blocks);
                kfree(tf);
            }
            tmpfs_remove_dirent(new_parent, new_name);
        }
        tmpfs_add_dirent(new_parent, new_name, d->file);
        if (old_parent != new_parent || kstrcmp(old_name, new_name) != 0)
            tmpfs_remove_dirent(old_parent, old_name);
        spinlock_release(&old_fs->lock, _sflags);
        return 0;
    }

    /* Cross-filesystem rename: lock both (order by address to prevent deadlock) */
    if ((uintptr_t)old_fs < (uintptr_t)new_fs) {
        cpu_flags_t _s1; spinlock_acquire(&old_fs->lock, &_s1);
        cpu_flags_t _s2; spinlock_acquire(&new_fs->lock, &_s2);
        tmpfs_dirent_t* d = tmpfs_lookup(old_parent, old_name);
        if (!d) { spinlock_release(&new_fs->lock, _s2); spinlock_release(&old_fs->lock, _s1); return -1; }
        tmpfs_dirent_t* t = tmpfs_lookup(new_parent, new_name);
        if (t) {
            if (t->file == d->file) { spinlock_release(&new_fs->lock, _s2); spinlock_release(&old_fs->lock, _s1); return 0; }
            tmpfs_file_t* tf = t->file;
            if (tf->nlink > 1) { tf->nlink--; }
            else {
                for (uint32_t i = 0; i < tf->nblocks; i++)
                    if (tf->blocks[i]) pmm_free_page(tf->blocks[i]);
                if (tf->blocks) kfree(tf->blocks);
                kfree(tf);
            }
            tmpfs_remove_dirent(new_parent, new_name);
        }
        tmpfs_add_dirent(new_parent, new_name, d->file);
        tmpfs_remove_dirent(old_parent, old_name);
        spinlock_release(&new_fs->lock, _s2);
        spinlock_release(&old_fs->lock, _s1);
        return 0;
    } else {
        cpu_flags_t _s1; spinlock_acquire(&new_fs->lock, &_s1);
        cpu_flags_t _s2; spinlock_acquire(&old_fs->lock, &_s2);
        tmpfs_dirent_t* d = tmpfs_lookup(old_parent, old_name);
        if (!d) { spinlock_release(&old_fs->lock, _s2); spinlock_release(&new_fs->lock, _s1); return -1; }
        tmpfs_dirent_t* t = tmpfs_lookup(new_parent, new_name);
        if (t) {
            if (t->file == d->file) { spinlock_release(&old_fs->lock, _s2); spinlock_release(&new_fs->lock, _s1); return 0; }
            tmpfs_file_t* tf = t->file;
            if (tf->nlink > 1) { tf->nlink--; }
            else {
                for (uint32_t i = 0; i < tf->nblocks; i++)
                    if (tf->blocks[i]) pmm_free_page(tf->blocks[i]);
                if (tf->blocks) kfree(tf->blocks);
                kfree(tf);
            }
            tmpfs_remove_dirent(new_parent, new_name);
        }
        tmpfs_add_dirent(new_parent, new_name, d->file);
        tmpfs_remove_dirent(old_parent, old_name);
        spinlock_release(&old_fs->lock, _s2);
        spinlock_release(&new_fs->lock, _s1);
        return 0;
    }
}

static int tmpfs_vfs_link(vfs_node_t* dir, const char* name, vfs_node_t* target) {
    tmpfs_file_t* parent = (tmpfs_file_t*)dir->private_data;
    tmpfs_file_t* tgt = (tmpfs_file_t*)target->private_data;
    if (!parent || !tgt) return -1;
    cpu_flags_t _sflags;
    spinlock_acquire(&tmpfs_from_node(dir)->lock, &_sflags);

    if (tmpfs_lookup(parent, name)) { spinlock_release(&tmpfs_from_node(dir)->lock, _sflags); return -1; }

    if (tmpfs_add_dirent(parent, name, tgt) < 0) { spinlock_release(&tmpfs_from_node(dir)->lock, _sflags); return -1; }
    tgt->nlink++;
    spinlock_release(&tmpfs_from_node(dir)->lock, _sflags);
    return 0;
}

static vfs_file_ops_t tmpfs_ops = {
    .open    = tmpfs_vfs_open,
    .close   = tmpfs_vfs_close,
    .read    = tmpfs_vfs_read,
    .write   = tmpfs_vfs_write,
    .readdir = tmpfs_vfs_readdir,
    .create  = tmpfs_vfs_create,
    .unlink  = tmpfs_vfs_unlink,
    .rename  = tmpfs_vfs_rename,
    .link    = tmpfs_vfs_link,
    .stat    = tmpfs_vfs_stat,
    .truncate = tmpfs_vfs_truncate,
    .chmod   = tmpfs_vfs_chmod,
    .lock    = tmpfs_vfs_lock,
    .unlock  = tmpfs_vfs_unlock,
};

err_t tmpfs_mount(vfs_fs_t** out_fs) {
    tmpfs_fs_t* fs = kmalloc(sizeof(tmpfs_fs_t));
    if (!fs) return ERR_NOMEM;
    kmemset(fs, 0, sizeof(tmpfs_fs_t));

    fs->root_dir = tmpfs_create_file(TMPFS_TYPE_DIR);
    if (!fs->root_dir) { kfree(fs); return ERR_NOMEM; }
    spinlock_init(&fs->lock, "tmpfs");

    kstrncpy(fs->vfs_fs.name, "tmpfs", sizeof(fs->vfs_fs.name) - 1);
    fs->vfs_fs.root = &fs->root_node;
    fs->vfs_fs.ops = &tmpfs_ops;

    kmemset(&fs->root_node, 0, sizeof(fs->root_node));
    kstrncpy(fs->root_node.name, "/", VFS_MAX_NAME - 1);
    fs->root_node.flags = 1;
    fs->root_node.size = 0;
    fs->root_node.fs = &fs->vfs_fs;
    fs->root_node.private_data = fs->root_dir;

    err_t e = vfs_mount("/tmp", &fs->vfs_fs);
    if (e) { kfree(fs); return e; }

    kprintf("[TMPFS] Mounted at /tmp\n");
    *out_fs = &fs->vfs_fs;
    return ERR_OK;
}
