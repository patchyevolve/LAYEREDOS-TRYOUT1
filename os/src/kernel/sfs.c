#include "kernel.h"
#include "sfs.h"
#include "block.h"
#include "pmm.h"
#include "kmalloc.h"
#include "hal.h"

#define SFS_INODES_PER_BLOCK (SFS_BLOCK_SIZE / sizeof(sfs_inode_t))
#define SFS_DIRENTS_PER_BLOCK (SFS_BLOCK_SIZE / sizeof(sfs_dirent_t))

static int sfs_blocks_for_inodes(uint32_t count) {
    return (count + SFS_INODES_PER_BLOCK - 1) / SFS_INODES_PER_BLOCK;
}

static int sfs_blocks_for_bmap(uint32_t bits) {
    return (bits + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
}

static err_t sfs_read_block(sfs_fs_t* fs, uint32_t block, void* buf) {
    return block_read(fs->bdev, block, 1, buf);
}

static err_t sfs_write_block(sfs_fs_t* fs, uint32_t block, const void* buf) {
    return block_write(fs->bdev, block, 1, buf);
}

static int sfs_bmap_alloc(sfs_fs_t* fs, uint32_t bmap_start, uint32_t total) {
    uint8_t buf[SFS_BLOCK_SIZE];
    uint32_t bmap_blocks = sfs_blocks_for_bmap(total);
    for (uint32_t b = 0; b < bmap_blocks; b++) {
        if (sfs_read_block(fs, bmap_start + b, buf) != ERR_OK)
            return -1;
        for (uint32_t i = 0; i < SFS_BLOCK_SIZE * 8; i++) {
            if (!(buf[i / 8] & (1 << (i % 8)))) {
                buf[i / 8] |= (1 << (i % 8));
                sfs_write_block(fs, bmap_start + b, buf);
                return (int)(b * SFS_BLOCK_SIZE * 8 + i);
            }
        }
    }
    return -1;
}

static void sfs_bmap_free(sfs_fs_t* fs, uint32_t bmap_start, uint32_t idx) {
    uint32_t block = idx / (SFS_BLOCK_SIZE * 8);
    uint32_t bit = idx % (SFS_BLOCK_SIZE * 8);
    uint8_t buf[SFS_BLOCK_SIZE];
    if (sfs_read_block(fs, bmap_start + block, buf) != ERR_OK) return;
    buf[bit / 8] &= ~(1 << (bit % 8));
    sfs_write_block(fs, bmap_start + block, buf);
}

static int sfs_alloc_inode(sfs_fs_t* fs) {
    int idx = sfs_bmap_alloc(fs, fs->sb.inode_bmap_start, fs->sb.total_inodes);
    if (idx < 0) return -1;
    sfs_inode_t inode;
    kmemset(&inode, 0, sizeof(inode));
    uint32_t block = fs->sb.inode_table_start + idx / SFS_INODES_PER_BLOCK;
    uint32_t off = idx % SFS_INODES_PER_BLOCK;
    uint8_t buf[SFS_BLOCK_SIZE];
    sfs_read_block(fs, block, buf);
    kmemcpy(buf + off * sizeof(sfs_inode_t), &inode, sizeof(sfs_inode_t));
    sfs_write_block(fs, block, buf);
    return idx;
}

static void sfs_free_inode(sfs_fs_t* fs, int idx) {
    if (idx < 0) return;
    sfs_bmap_free(fs, fs->sb.inode_bmap_start, (uint32_t)idx);
}

static int sfs_alloc_block(sfs_fs_t* fs) {
    uint32_t total_data = fs->sb.total_blocks - fs->sb.data_start;
    int idx = sfs_bmap_alloc(fs, fs->sb.block_bmap_start, total_data);
    if (idx < 0) return -1;
    return (int)(fs->sb.data_start + idx);
}

static void sfs_free_block(sfs_fs_t* fs, uint32_t block) {
    if (block < fs->sb.data_start) return;
    uint32_t idx = block - fs->sb.data_start;
    sfs_bmap_free(fs, fs->sb.block_bmap_start, idx);
}

err_t sfs_read_inode(sfs_fs_t* fs, int inum, sfs_inode_t* inode) {
    uint32_t block = fs->sb.inode_table_start + inum / SFS_INODES_PER_BLOCK;
    uint32_t off = inum % SFS_INODES_PER_BLOCK;
    uint8_t buf[SFS_BLOCK_SIZE];
    err_t e = sfs_read_block(fs, block, buf);
    if (e) return e;
    kmemcpy(inode, buf + off * sizeof(sfs_inode_t), sizeof(sfs_inode_t));
    return ERR_OK;
}

err_t sfs_write_inode(sfs_fs_t* fs, int inum, const sfs_inode_t* inode) {
    uint32_t block = fs->sb.inode_table_start + inum / SFS_INODES_PER_BLOCK;
    uint32_t off = inum % SFS_INODES_PER_BLOCK;
    uint8_t buf[SFS_BLOCK_SIZE];
    err_t e = sfs_read_block(fs, block, buf);
    if (e) return e;
    kmemcpy(buf + off * sizeof(sfs_inode_t), inode, sizeof(sfs_inode_t));
    return sfs_write_block(fs, block, buf);
}

static err_t sfs_read_data(sfs_fs_t* fs, uint32_t block, void* buf) {
    return sfs_read_block(fs, block, buf);
}

static err_t sfs_write_data(sfs_fs_t* fs, uint32_t block, const void* buf) {
    return sfs_write_block(fs, block, buf);
}

#define SFS_INDIRECT_PTRS (SFS_BLOCK_SIZE / 4)

static int sfs_inode_get_block(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t file_block, int create) {
    if (file_block < SFS_DIRECT_BLOCKS) {
        if (inode->direct[file_block] == 0) {
            if (!create) return -1;
            int b = sfs_alloc_block(fs);
            if (b < 0) return -1;
            inode->direct[file_block] = (uint32_t)b;
        }
        return (int)inode->direct[file_block];
    }

    /* Indirect blocks */
    uint32_t indirect_idx = file_block - SFS_DIRECT_BLOCKS;
    if (indirect_idx >= SFS_INDIRECT_PTRS) return -1;

    if (inode->indirect == 0) {
        if (!create) return -1;
        int ib = sfs_alloc_block(fs);
        if (ib < 0) return -1;
        inode->indirect = (uint32_t)ib;
    }

    uint8_t ibuf[SFS_BLOCK_SIZE];
    if (create)
        kmemset(ibuf, 0, SFS_BLOCK_SIZE);
    if (inode->indirect != 0) {
        err_t e = sfs_read_data(fs, inode->indirect, ibuf);
        if (e && !create) return -1;
    }

    uint32_t* ptrs = (uint32_t*)ibuf;
    if (ptrs[indirect_idx] == 0) {
        if (!create) return -1;
        int b = sfs_alloc_block(fs);
        if (b < 0) return -1;
        ptrs[indirect_idx] = (uint32_t)b;
        sfs_write_data(fs, inode->indirect, ibuf);
    }
    return (int)ptrs[indirect_idx];
}

static err_t sfs_readlink(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t target_block, uint32_t file_off, void* buf, uint32_t count) {
    uint8_t tmp[SFS_BLOCK_SIZE];
    err_t e = sfs_read_data(fs, target_block, tmp);
    if (e) return e;
    uint32_t copy = count;
    if (file_off + copy > inode->size) copy = inode->size - file_off;
    kmemcpy(buf, tmp + file_off, copy);
    return ERR_OK;
}

static err_t sfs_writelink(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t target_block, uint32_t file_off, const void* buf, uint32_t count) {
    uint8_t tmp[SFS_BLOCK_SIZE];
    if (count < SFS_BLOCK_SIZE) {
        err_t e = sfs_read_data(fs, target_block, tmp);
        if (e) kmemset(tmp, 0, SFS_BLOCK_SIZE);
    }
    kmemcpy(tmp + file_off, buf, count);
    return sfs_write_data(fs, target_block, tmp);
}

/* VFS operations - forward declarations */
static int sfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir);
static int sfs_vfs_unlink(vfs_node_t* dir, const char* name);

typedef struct sfs_file {
    sfs_fs_t* fs;
    int inum;
    sfs_inode_t inode;
    uint32_t offset;
} sfs_file_t;

static int sfs_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int sfs_vfs_close(vfs_node_t* node) {
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (f) {
        kfree(f);
        node->private_data = NULL;
    }
    return 0;
}

static int64_t sfs_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) return -1;

    uint64_t remain = f->inode.size > offset ? f->inode.size - offset : 0;
    if (count > remain) count = remain;
    if (count == 0) return 0;

    uint64_t done = 0;
    while (done < count) {
        uint32_t file_block = (uint32_t)((offset + done) / SFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((offset + done) % SFS_BLOCK_SIZE);
        uint32_t chunk = SFS_BLOCK_SIZE - block_off;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        int phys = sfs_inode_get_block(f->fs, &f->inode, file_block, 0);
        if (phys < 0) break;

        if (sfs_readlink(f->fs, &f->inode, (uint32_t)phys, block_off, (uint8_t*)buf + done, chunk))
            break;
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t sfs_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) return -1;

    uint64_t done = 0;
    while (done < count) {
        uint32_t file_block = (uint32_t)((offset + done) / SFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((offset + done) % SFS_BLOCK_SIZE);
        uint32_t chunk = SFS_BLOCK_SIZE - block_off;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        int phys = sfs_inode_get_block(f->fs, &f->inode, file_block, 1);
        if (phys < 0) {
            uint64_t partial_new_size = offset + done;
            if (partial_new_size > f->inode.size) {
                f->inode.size = (uint32_t)partial_new_size;
            }
            f->inode.mtime = (uint32_t)hal_timer_get_ticks();
            sfs_write_inode(f->fs, f->inum, &f->inode);
            node->size = f->inode.size;
            return (int64_t)done;
        }

        if (sfs_writelink(f->fs, &f->inode, (uint32_t)phys, block_off, (const uint8_t*)buf + done, chunk))
            return (int64_t)done;
        done += chunk;
    }

    uint64_t new_size = offset + done;
    if (new_size > f->inode.size) {
        f->inode.size = (uint32_t)new_size;
    }
    f->inode.mtime = (uint32_t)hal_timer_get_ticks();
    sfs_write_inode(f->fs, f->inum, &f->inode);
    node->size = f->inode.size;
    return (int64_t)done;
}

static int sfs_vfs_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f || f->inode.type != SFS_TYPE_DIR) return -1;

    sfs_read_inode(f->fs, f->inum, &f->inode);
    uint32_t max_entries = f->inode.size / sizeof(sfs_dirent_t);
    if (index >= max_entries) return -1;

    uint32_t entry_block = index / SFS_DIRENTS_PER_BLOCK;
    uint32_t entry_off = index % SFS_DIRENTS_PER_BLOCK;
    uint32_t file_block_num = entry_block;

    int phys = sfs_inode_get_block(f->fs, &f->inode, file_block_num, 0);
    if (phys < 0) return -1;

    uint8_t tmp[SFS_BLOCK_SIZE];
    err_t e = sfs_read_data(f->fs, (uint32_t)phys, tmp);
    if (e) return -1;

    sfs_dirent_t* de = (sfs_dirent_t*)(tmp + entry_off * sizeof(sfs_dirent_t));
    if (de->inode == 0) return -1;

    vfs_node_t* child = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!child) return -1;
    kmemset(child, 0, sizeof(vfs_node_t));
    kstrncpy(child->name, de->name, VFS_MAX_NAME - 1);

    sfs_inode_t cinode;
    sfs_read_inode(f->fs, (int)de->inode, &cinode);
    child->size = cinode.size;
    child->flags = (cinode.type == SFS_TYPE_DIR) ? 1 : 0;
    if (cinode.type == SFS_TYPE_SYMLINK) child->flags |= VFS_FLAG_SYMLINK;
    child->fs = &f->fs->vfs_fs;

    sfs_file_t* cf = kmalloc(sizeof(sfs_file_t));
    if (cf) {
        cf->fs = f->fs;
        cf->inum = (int)de->inode;
        cf->offset = 0;
        sfs_read_inode(f->fs, cf->inum, &cf->inode);
        child->private_data = cf;
    }

    *out = child;
    return 0;
}

static int sfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !st || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    st->size  = f->inode.size;
    st->inode = f->inum;
    st->mode  = f->inode.mode | (f->inode.type << 16);
    st->flags = node->flags;
    st->atime = f->inode.atime;
    st->mtime = f->inode.mtime;
    st->ctime = f->inode.ctime;
    st->fs_flags = f->inode.flags;
    return 0;
}

static int sfs_vfs_truncate(vfs_node_t* node, uint64_t size) {
    if (!node || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (size >= f->inode.size) return 0;

    uint32_t new_blocks = (uint32_t)((size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE);
    uint32_t old_blocks = (uint32_t)((f->inode.size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE);

    for (uint32_t b = new_blocks; b < old_blocks && b < SFS_DIRECT_BLOCKS; b++) {
        if (f->inode.direct[b]) {
            sfs_free_block(f->fs, f->inode.direct[b]);
            f->inode.direct[b] = 0;
        }
    }
    if (f->inode.indirect) {
        uint32_t indirect_start = SFS_DIRECT_BLOCKS;
        uint32_t freed_indirect = 0;
        uint32_t kept = 0;
        uint8_t ibuf[SFS_BLOCK_SIZE];
        sfs_read_data(f->fs, f->inode.indirect, ibuf);
        uint32_t* ptrs = (uint32_t*)ibuf;
        for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
            uint32_t blk = indirect_start + i;
            if (ptrs[i]) {
                if (blk < new_blocks) {
                    kept++;
                } else {
                    sfs_free_block(f->fs, ptrs[i]);
                    ptrs[i] = 0;
                    freed_indirect++;
                }
            }
        }
        if (freed_indirect > 0) {
            if (kept == 0) {
                sfs_free_block(f->fs, f->inode.indirect);
                f->inode.indirect = 0;
            } else {
                sfs_write_data(f->fs, f->inode.indirect, ibuf);
            }
        }
    }
    f->inode.size = (uint32_t)size;
    f->inode.mtime = (uint32_t)hal_timer_get_ticks();
    sfs_write_inode(f->fs, f->inum, &f->inode);
    node->size = f->inode.size;
    return 0;
}

static int sfs_vfs_chmod(vfs_node_t* node, uint32_t mode) {
    if (!node || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    f->inode.mode = mode & 0xFFFF;
    sfs_write_inode(f->fs, f->inum, &f->inode);
    return 0;
}

static int sfs_vfs_lock(vfs_node_t* node) {
    if (!node || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    f->inode.flags |= SFS_INODE_FLAG_LOCKED;
    sfs_write_inode(f->fs, f->inum, &f->inode);
    return 0;
}

static int sfs_vfs_unlock(vfs_node_t* node) {
    if (!node || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    f->inode.flags &= ~SFS_INODE_FLAG_LOCKED;
    sfs_write_inode(f->fs, f->inum, &f->inode);
    return 0;
}

static int sfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                          vfs_node_t* new_dir, const char* new_name);
static int sfs_vfs_link(vfs_node_t* dir, const char* name, vfs_node_t* target);
static int sfs_vfs_symlink(vfs_node_t* dir, const char* name, const char* target);
static int sfs_vfs_readlink(vfs_node_t* node, char* buf, uint64_t size);

static vfs_file_ops_t sfs_ops = {
    .open    = sfs_vfs_open,
    .close   = sfs_vfs_close,
    .read    = sfs_vfs_read,
    .write   = sfs_vfs_write,
    .readdir = sfs_vfs_readdir,
    .create  = sfs_vfs_create,
    .unlink  = sfs_vfs_unlink,
    .link    = sfs_vfs_link,
    .rename  = sfs_vfs_rename,
    .stat    = sfs_vfs_stat,
    .truncate = sfs_vfs_truncate,
    .chmod   = sfs_vfs_chmod,
    .lock    = sfs_vfs_lock,
    .unlock  = sfs_vfs_unlock,
    .symlink = sfs_vfs_symlink,
    .readlink = sfs_vfs_readlink,
};

static int sfs_lookup(sfs_fs_t* fs, int dir_inum, const char* name) {
    sfs_inode_t dir_inode;
    if (sfs_read_inode(fs, dir_inum, &dir_inode) != ERR_OK) return -1;
    if (dir_inode.type != SFS_TYPE_DIR) return -1;

    uint32_t max_entries = dir_inode.size / sizeof(sfs_dirent_t);
    uint8_t tmp[SFS_BLOCK_SIZE];

    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t bi = i / SFS_DIRENTS_PER_BLOCK;
        uint32_t off = i % SFS_DIRENTS_PER_BLOCK;
        if (off == 0) {
            int phys = sfs_inode_get_block(fs, &dir_inode, bi, 0);
            if (phys < 0) return -1;
            if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) return -1;
        }
        sfs_dirent_t* de = (sfs_dirent_t*)tmp + off;
        if (de->inode == 0) continue;
        if (kstrcmp(de->name, name) == 0) return (int)de->inode;
    }
    return -1;
}

static err_t sfs_add_dirent(sfs_fs_t* fs, int dir_inum, const char* name, int child_inum) {
    vfs_node_t parent_node;
    sfs_file_t parent_file;
    kmemset(&parent_node, 0, sizeof(parent_node));
    kmemset(&parent_file, 0, sizeof(parent_file));

    parent_file.fs = fs;
    parent_file.inum = dir_inum;
    parent_file.offset = 0;
    sfs_read_inode(fs, dir_inum, &parent_file.inode);

    if (parent_file.inode.type != SFS_TYPE_DIR) return ERR_INVAL;

    uint32_t size = parent_file.inode.size;

    sfs_dirent_t de;
    kmemset(&de, 0, sizeof(de));
    de.inode = (uint32_t)child_inum;
    kstrncpy(de.name, name, SFS_NAME_MAX - 1);

    parent_node.fs = &fs->vfs_fs;
    parent_node.private_data = &parent_file;

    int64_t ret = sfs_vfs_write(&parent_node, &de, sizeof(de), size);
    if (ret < 0) return ERR_NOSPACE;
    return ERR_OK;
}

static err_t sfs_remove_dirent(sfs_fs_t* fs, int dir_inum, const char* name) {
    sfs_inode_t dir_inode;
    if (sfs_read_inode(fs, dir_inum, &dir_inode) != ERR_OK) return ERR_INVAL;

    uint32_t max_entries = dir_inode.size / sizeof(sfs_dirent_t);
    int found_idx = -1;

    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t file_block = i / SFS_DIRENTS_PER_BLOCK;
        uint32_t entry_off = i % SFS_DIRENTS_PER_BLOCK;
        int phys = sfs_inode_get_block(fs, &dir_inode, file_block, 0);
        if (phys < 0) return ERR_NOENT;
        uint8_t tmp[SFS_BLOCK_SIZE];
        if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) return ERR_IO;
        sfs_dirent_t* de = (sfs_dirent_t*)tmp + entry_off;
        if (de->inode != 0 && kstrcmp(de->name, name) == 0) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) return ERR_NOENT;

    int last_entry = (int)max_entries - 1;
    /* Compact: shift all subsequent entries back by one */
    for (int i = found_idx; i < last_entry; i++) {
        uint32_t src_block = (i + 1) / 16;
        uint32_t src_off   = (i + 1) % 16;
        uint32_t dst_block = i / 16;
        uint32_t dst_off   = i % 16;

        uint8_t buf[SFS_BLOCK_SIZE];
        int phys = sfs_inode_get_block(fs, &dir_inode, src_block, 0);
        if (phys < 0) break;
        if (sfs_read_data(fs, (uint32_t)phys, buf)) break;
        sfs_dirent_t* sde = (sfs_dirent_t*)buf + src_off;
        if (sde->inode == 0) { sde = NULL; continue; }

        if (src_block == dst_block) {
            kmemcpy(buf + dst_off * sizeof(sfs_dirent_t),
                    buf + src_off * sizeof(sfs_dirent_t),
                    sizeof(sfs_dirent_t));
            kmemset(buf + src_off * sizeof(sfs_dirent_t), 0, sizeof(sfs_dirent_t));
            sfs_write_data(fs, (uint32_t)phys, buf);
        } else {
            uint8_t dbuf[SFS_BLOCK_SIZE];
            int dphys = sfs_inode_get_block(fs, &dir_inode, dst_block, 0);
            if (dphys < 0) break;
            if (sfs_read_data(fs, (uint32_t)dphys, dbuf)) break;
            sfs_dirent_t* dde = (sfs_dirent_t*)dbuf + dst_off;
            kmemcpy(dde, sde, sizeof(sfs_dirent_t));
            if (sfs_write_data(fs, (uint32_t)dphys, dbuf)) break;
            kmemset(buf + src_off * sizeof(sfs_dirent_t), 0, sizeof(sfs_dirent_t));
            sfs_write_data(fs, (uint32_t)phys, buf);
        }
    }

    uint32_t new_size = dir_inode.size - sizeof(sfs_dirent_t);
    dir_inode.size = new_size;
    sfs_write_inode(fs, dir_inum, &dir_inode);

    return ERR_OK;
}

static int sfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir) {
    if (!dir || !name) return -1;
    sfs_file_t* f = (sfs_file_t*)dir->private_data;
    if (!f) { kprintf("[SFS_CREATE] no private_data\n"); return -1; }
    if (f->inode.type != SFS_TYPE_DIR) {
        kprintf("[SFS_CREATE] not a dir, type=%u\n", f->inode.type);
        return -1;
    }

    if (sfs_lookup(f->fs, f->inum, name) >= 0) return -1;

    int inum = sfs_alloc_inode(f->fs);
    if (inum < 0) return -1;

    sfs_inode_t inode;
    kmemset(&inode, 0, sizeof(inode));
    inode.type = (uint16_t)(is_dir ? SFS_TYPE_DIR : SFS_TYPE_FILE);
    inode.mode = 0644;
    inode.size = 0;
    uint64_t now = hal_timer_get_ticks();
    inode.atime = (uint32_t)now;
    inode.mtime = (uint32_t)now;
    inode.ctime = (uint32_t)now;
    inode.nlink = 1;
    sfs_write_inode(f->fs, inum, &inode);

    if (sfs_add_dirent(f->fs, f->inum, name, inum) != ERR_OK) {
        sfs_free_inode(f->fs, inum);
        return -1;
    }

    return inum;
}

static int sfs_vfs_unlink(vfs_node_t* dir, const char* name) {
    if (!dir || !name) return -1;
    sfs_file_t* f = (sfs_file_t*)dir->private_data;
    if (!f) return -1;

    int inum = sfs_lookup(f->fs, f->inum, name);
    if (inum < 0) return -1;

    sfs_inode_t inode;
    sfs_read_inode(f->fs, inum, &inode);

    err_t e = sfs_remove_dirent(f->fs, f->inum, name);
    if (e) return -1;

    if (inode.nlink > 1) {
        inode.nlink--;
        sfs_write_inode(f->fs, inum, &inode);
        return 0;
    }

    /* Last link: free blocks and inode */
    for (int i = 0; i < SFS_DIRECT_BLOCKS; i++) {
        if (inode.direct[i])
            sfs_free_block(f->fs, inode.direct[i]);
    }
    if (inode.indirect) {
        uint8_t ibuf[SFS_BLOCK_SIZE];
        if (sfs_read_data(f->fs, inode.indirect, ibuf) == ERR_OK) {
            uint32_t* ptrs = (uint32_t*)ibuf;
            for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                if (ptrs[i]) sfs_free_block(f->fs, ptrs[i]);
            }
        }
        sfs_free_block(f->fs, inode.indirect);
    }

    sfs_free_inode(f->fs, inum);
    return 0;
}

static int sfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                          vfs_node_t* new_dir, const char* new_name) {
    if (!old_dir || !old_name || !new_dir || !new_name) return -1;
    sfs_file_t* old_f = (sfs_file_t*)old_dir->private_data;
    sfs_file_t* new_f = (sfs_file_t*)new_dir->private_data;
    if (!old_f || !new_f) return -1;

    int inum = sfs_lookup(old_f->fs, old_f->inum, old_name);
    if (inum < 0) return -1;

    /* Fail if target already exists */
    if (sfs_lookup(new_f->fs, new_f->inum, new_name) >= 0) return -1;

    if (sfs_add_dirent(new_f->fs, new_f->inum, new_name, inum) != ERR_OK)
        return -1;
    sfs_remove_dirent(old_f->fs, old_f->inum, old_name);
    return 0;
}

static int sfs_vfs_symlink(vfs_node_t* dir, const char* name, const char* target) {
    if (!dir || !name || !target) return -1;
    sfs_file_t* dir_f = (sfs_file_t*)dir->private_data;
    if (!dir_f) return -1;

    if (sfs_lookup(dir_f->fs, dir_f->inum, name) >= 0) return -1;

    int inum = sfs_alloc_inode(dir_f->fs);
    if (inum < 0) return -1;

    sfs_inode_t inode;
    kmemset(&inode, 0, sizeof(inode));
    inode.type = SFS_TYPE_SYMLINK;
    inode.mode = 0777;
    uint64_t now = hal_timer_get_ticks();
    inode.atime = (uint32_t)now;
    inode.mtime = (uint32_t)now;
    inode.ctime = (uint32_t)now;
    inode.nlink = 1;

    /* Write target path into data blocks using a temporary vfs_node */
    vfs_node_t tmp_node;
    kmemset(&tmp_node, 0, sizeof(tmp_node));
    sfs_file_t tmp_f;
    tmp_f.fs = dir_f->fs;
    tmp_f.inum = inum;
    tmp_f.offset = 0;
    tmp_f.inode = inode;
    tmp_node.private_data = &tmp_f;
    tmp_node.size = 0;

    int64_t written = sfs_vfs_write(&tmp_node, target, kstrlen(target), 0);
    if (written < 0) {
        sfs_free_inode(dir_f->fs, inum);
        return -1;
    }
    inode = tmp_f.inode;

    sfs_write_inode(dir_f->fs, inum, &inode);

    if (sfs_add_dirent(dir_f->fs, dir_f->inum, name, inum) != ERR_OK) {
        sfs_free_inode(dir_f->fs, inum);
        return -1;
    }

    return 0;
}

static int sfs_vfs_readlink(vfs_node_t* node, char* buf, uint64_t size) {
    if (!node || !buf || !node->private_data) return -1;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (f->inode.type != SFS_TYPE_SYMLINK) return -1;

    uint32_t to_read = f->inode.size;
    if (to_read >= size) to_read = (uint32_t)(size - 1);
    if (to_read == 0) { buf[0] = '\0'; return 0; }

    /* Read from data blocks */
    vfs_node_t tmp_node;
    kmemset(&tmp_node, 0, sizeof(tmp_node));
    sfs_file_t tmp_f = *f;
    tmp_node.private_data = &tmp_f;

    int64_t r = sfs_vfs_read(&tmp_node, buf, to_read, 0);
    if (r < 0) return -1;
    buf[r] = '\0';
    return 0;
}

static int sfs_vfs_link(vfs_node_t* dir, const char* name, vfs_node_t* target) {
    if (!dir || !name || !target) return -1;
    sfs_file_t* dir_f = (sfs_file_t*)dir->private_data;
    sfs_file_t* tgt_f = (sfs_file_t*)target->private_data;
    if (!dir_f || !tgt_f) return -1;

    if (sfs_lookup(dir_f->fs, dir_f->inum, name) >= 0) return -1;

    int inum = tgt_f->inum;
    if (sfs_add_dirent(dir_f->fs, dir_f->inum, name, inum) != ERR_OK)
        return -1;

    sfs_inode_t inode;
    sfs_read_inode(tgt_f->fs, inum, &inode);
    inode.nlink++;
    sfs_write_inode(tgt_f->fs, inum, &inode);
    return 0;
}

err_t sfs_format(block_dev_t* bdev) {
    uint64_t total_blocks = bdev->block_count;
    if (total_blocks < 16) return ERR_NOSPACE;

    sfs_superblock_t sb;
    kmemset(&sb, 0, sizeof(sb));
    sb.magic = SFS_MAGIC;
    sb.total_inodes = SFS_MAX_INODES;
    sb.total_blocks = (uint32_t)total_blocks;
    sb.inode_bmap_start = 1;
    sb.block_bmap_start = sb.inode_bmap_start + sfs_blocks_for_bmap(SFS_MAX_INODES);
    sb.inode_table_start = sb.block_bmap_start + sfs_blocks_for_bmap((uint32_t)total_blocks);
    uint32_t inode_table_blocks = (uint32_t)sfs_blocks_for_inodes(SFS_MAX_INODES);
    sb.data_start = sb.inode_table_start + inode_table_blocks;
    sb.root_inode = 0;

    uint8_t zero[SFS_BLOCK_SIZE];
    kmemset(zero, 0, SFS_BLOCK_SIZE);

    block_write(bdev, 0, 1, &sb);

    uint32_t meta_end = sb.data_start;
    for (uint32_t b = 1; b < meta_end; b++)
        block_write(bdev, b, 1, zero);

    {
        uint8_t bmap_buf[SFS_BLOCK_SIZE];
        block_read(bdev, sb.inode_bmap_start, 1, bmap_buf);
        bmap_buf[0] |= 1;
        block_write(bdev, sb.inode_bmap_start, 1, bmap_buf);
    }

    sfs_inode_t root_inode;
    kmemset(&root_inode, 0, sizeof(root_inode));
    root_inode.type = SFS_TYPE_DIR;
    root_inode.size = 0;

    uint32_t inode_block = sb.inode_table_start;
    uint8_t inode_buf[SFS_BLOCK_SIZE];
    block_read(bdev, inode_block, 1, inode_buf);
    kmemcpy(inode_buf, &root_inode, sizeof(sfs_inode_t));
    block_write(bdev, inode_block, 1, inode_buf);

    return ERR_OK;
}

err_t sfs_mount(block_dev_t* bdev) {
    sfs_fs_t* fs = kmalloc(sizeof(sfs_fs_t));
    if (!fs) return ERR_NOMEM;
    kmemset(fs, 0, sizeof(sfs_fs_t));

    fs->bdev = bdev;

    err_t e = block_read(bdev, 0, 1, &fs->sb);
    if (e) { kfree(fs); return e; }

    if (fs->sb.magic != SFS_MAGIC) {
        kprintf("[SFS] Bad magic 0x%x, need to format\n", fs->sb.magic);
        kfree(fs);
        return ERR_INVAL;
    }

    kstrncpy(fs->vfs_fs.name, "sfs", sizeof(fs->vfs_fs.name) - 1);
    fs->vfs_fs.root = &fs->root_node;
    fs->vfs_fs.ops = &sfs_ops;

    sfs_file_t* rf = kmalloc(sizeof(sfs_file_t));
    if (!rf) { kfree(fs); return ERR_NOMEM; }
    rf->fs = fs;
    rf->inum = (int)fs->sb.root_inode;
    rf->offset = 0;
    sfs_read_inode(fs, (int)fs->sb.root_inode, &rf->inode);

    kmemset(&fs->root_node, 0, sizeof(fs->root_node));
    kstrncpy(fs->root_node.name, "/", VFS_MAX_NAME - 1);
    fs->root_node.flags = 1;
    fs->root_node.size = rf->inode.size;
    fs->root_node.fs = &fs->vfs_fs;
    fs->root_node.private_data = rf;

    vfs_register_fs(&fs->vfs_fs);

    kprintf("[SFS] Mounted on '%s' (%u blocks, %u inodes)\n",
            bdev->name, fs->sb.total_blocks, fs->sb.total_inodes);
    return ERR_OK;
}

int sfs_get_block_size(void) { return SFS_BLOCK_SIZE; }
