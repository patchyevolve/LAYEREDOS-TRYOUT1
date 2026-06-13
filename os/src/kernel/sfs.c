#include "kernel.h"
#include "sfs.h"
#include "block.h"
#include "journal.h"
#include "fsck.h"
#include "pmm.h"
#include "kmalloc.h"
#include "hal.h"
#include "sync.h"

static int sfs_blocks_for_inodes(uint32_t count) {
    return (count + SFS_INODES_PER_BLOCK - 1) / SFS_INODES_PER_BLOCK;
}

static int sfs_blocks_for_bmap(uint32_t bits) {
    return (bits + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
}

static uint32_t sfs_sb_checksum(const sfs_superblock_t* sb) {
    uint32_t c = 0;
    const uint32_t* p = (const uint32_t*)sb;
    for (uint32_t i = 0; i < sizeof(sfs_superblock_t) / sizeof(uint32_t); i++)
        c ^= p[i];
    return c;
}

static int sfs_sb_verify(const sfs_superblock_t* sb) {
    if (sb->magic != SFS_MAGIC) return 0;
    uint32_t stored = sb->checksum;
    ((sfs_superblock_t*)sb)->checksum = 0;
    int ok = (sfs_sb_checksum(sb) == stored);
    ((sfs_superblock_t*)sb)->checksum = stored;
    return ok;
}

static void sfs_sb_finalize(sfs_superblock_t* sb) {
    sb->checksum = 0;
    sb->checksum = sfs_sb_checksum(sb);
}

static err_t sfs_read_block(sfs_fs_t* fs, uint32_t block, void* buf) {
    return block_read(fs->bdev, block, 1, buf);
}

/* Journal-aware metadata write — logs then caches the write */
static err_t sfs_write_meta(sfs_fs_t* fs, uint32_t block, const void* buf) {
    if (fs->journal_active) {
        err_t e = journal_log(fs->bdev, fs->journal_start,
                              (uint32_t)fs->journal_seq, block, buf);
        if (e == ERR_NOSPACE) {
            journal_commit(fs->bdev, fs->journal_start,
                           (uint32_t)fs->journal_seq);
            journal_checkpoint(fs->bdev, fs->journal_start);
            uint32_t new_seq;
            journal_start_txn(fs->bdev, fs->journal_start, &new_seq);
            fs->journal_seq = (int)new_seq;
            e = journal_log(fs->bdev, fs->journal_start,
                            (uint32_t)fs->journal_seq, block, buf);
        }
        if (e) return e;
    }
    return block_write(fs->bdev, block, 1, buf);
}

/* Transaction boundaries for FS operations */
static err_t sfs_begin_op(sfs_fs_t* fs) {
    if (!fs->journal_active) return ERR_OK;
    if (fs->journal_nest > 0) {
        fs->journal_nest++;
        return ERR_OK;
    }
    err_t e = journal_start_txn(fs->bdev, fs->journal_start,
                                (uint32_t*)&fs->journal_seq);
    if (e) return e;
    fs->journal_nest = 1;
    return ERR_OK;
}

static err_t sfs_end_op(sfs_fs_t* fs) {
    if (!fs->journal_active) return ERR_OK;
    if (fs->journal_nest > 1) {
        fs->journal_nest--;
        return ERR_OK;
    }
    fs->journal_nest = 0;
    err_t e = journal_commit(fs->bdev, fs->journal_start,
                             (uint32_t)fs->journal_seq);
    if (e) return e;
    return journal_checkpoint(fs->bdev, fs->journal_start);
}

static int sfs_bmap_alloc(sfs_fs_t* fs, uint32_t bmap_start, uint32_t total) {
    mutex_lock(&fs->bmap_lock, (uint64_t)-1);
    uint8_t buf[SFS_BLOCK_SIZE];
    uint32_t bmap_blocks = sfs_blocks_for_bmap(total);
    for (uint32_t b = 0; b < bmap_blocks; b++) {
        if (sfs_read_block(fs, bmap_start + b, buf) != ERR_OK) {
            mutex_unlock(&fs->bmap_lock);
            return -1;
        }
        for (uint32_t i = 0; i < SFS_BLOCK_SIZE * 8; i++) {
            if (!(buf[i / 8] & (1 << (i % 8)))) {
                buf[i / 8] |= (1 << (i % 8));
                sfs_write_meta(fs, bmap_start + b, buf);
                mutex_unlock(&fs->bmap_lock);
                return (int)(b * SFS_BLOCK_SIZE * 8 + i);
            }
        }
    }
    mutex_unlock(&fs->bmap_lock);
    return -1;
}

static void sfs_bmap_free(sfs_fs_t* fs, uint32_t bmap_start, uint32_t idx) {
    mutex_lock(&fs->bmap_lock, (uint64_t)-1);
    uint32_t block = idx / (SFS_BLOCK_SIZE * 8);
    uint32_t bit = idx % (SFS_BLOCK_SIZE * 8);
    uint8_t buf[SFS_BLOCK_SIZE];
    if (sfs_read_block(fs, bmap_start + block, buf) != ERR_OK) { mutex_unlock(&fs->bmap_lock); return; }
    buf[bit / 8] &= ~(1 << (bit % 8));
    sfs_write_meta(fs, bmap_start + block, buf);
    mutex_unlock(&fs->bmap_lock);
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
    sfs_write_meta(fs, block, buf);
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
    if (fs->block_owner && idx < (int)fs->block_owner_count && fs->block_owner[idx]) {
        kprintf("[SFS] FATAL: block idx %d already claimed (double allocation detected!)\n", idx);
    }
    if (fs->block_owner) fs->block_owner[idx] = 1;
    return (int)(fs->sb.data_start + idx);
}

static void sfs_free_block(sfs_fs_t* fs, uint32_t block) {
    if (block < fs->sb.data_start) return;
    uint32_t idx = block - fs->sb.data_start;
    if (fs->block_owner && idx < fs->block_owner_count) {
        if (!fs->block_owner[idx])
            kprintf("[SFS] WARN: free unclaimed block %u (idx %u)\n", block, idx);
        fs->block_owner[idx] = 0;
    }
    sfs_bmap_free(fs, fs->sb.block_bmap_start, idx);
}

static void sfs_init_block_owner(sfs_fs_t* fs) {
    uint32_t tdb = fs->sb.total_blocks - fs->sb.data_start;
    if (fs->block_owner) kfree(fs->block_owner);
    fs->block_owner_count = tdb;
    fs->block_owner = (uint8_t*)kmalloc(tdb);
    if (!fs->block_owner) { fs->block_owner_count = 0; return; }
    kmemset(fs->block_owner, 0, tdb);

    uint32_t bmap_blocks = (tdb + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
    for (uint32_t b = 0; b < bmap_blocks; b++) {
        uint8_t buf[SFS_BLOCK_SIZE];
        if (block_read(fs->bdev, fs->sb.block_bmap_start + b, 1, buf) != ERR_OK) break;
        for (uint32_t i = 0; i < SFS_BLOCK_SIZE * 8; i++) {
            uint32_t entry = b * SFS_BLOCK_SIZE * 8 + i;
            if (entry >= tdb) break;
            if (buf[i / 8] & (1 << (i % 8)))
                fs->block_owner[entry] = 1;
        }
    }
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
    return sfs_write_meta(fs, block, buf);
}

err_t sfs_read_data(sfs_fs_t* fs, uint32_t block, void* buf) {
    return sfs_read_block(fs, block, buf);
}

/* SFS_INDIRECT_PTRS etc. defined in sfs.h */

int sfs_inode_get_block(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t file_block, int create) {
    if (file_block < SFS_DIRECT_BLOCKS) {
        if (inode->direct[file_block] == 0) {
            if (!create) return -1;
            int b = sfs_alloc_block(fs);
            if (b < 0) return -1;
            inode->direct[file_block] = (uint32_t)b;
        }
        return (int)inode->direct[file_block];
    }

    /* Singly-indirect blocks (file_block 12-139) */
    if (file_block < SFS_DINDIRECT_START) {
        uint32_t indirect_idx = file_block - SFS_DIRECT_BLOCKS;
        if (indirect_idx >= SFS_INDIRECT_PTRS) return -1;

        if (inode->indirect == 0) {
            if (!create) return -1;
            int ib = sfs_alloc_block(fs);
            if (ib < 0) return -1;
            inode->indirect = (uint32_t)ib;
            uint8_t z[SFS_BLOCK_SIZE];
            kmemset(z, 0, SFS_BLOCK_SIZE);
            sfs_write_meta(fs, inode->indirect, z);
        }

        uint8_t ibuf[SFS_BLOCK_SIZE];
        {
            err_t e = sfs_read_data(fs, inode->indirect, ibuf);
            if (e && !create) return -1;
        }

        uint32_t* ptrs = (uint32_t*)ibuf;
        if (ptrs[indirect_idx] == 0) {
            if (!create) return -1;
            int b = sfs_alloc_block(fs);
            if (b < 0) return -1;
            ptrs[indirect_idx] = (uint32_t)b;
            sfs_write_meta(fs, inode->indirect, ibuf);
        }
        return (int)ptrs[indirect_idx];
    }

    /* Doubly-indirect blocks (file_block 140-16523) */
    uint32_t dindirect_idx = file_block - SFS_DINDIRECT_START;
    if (dindirect_idx >= SFS_DINDIRECT_PTRS) return -1;

    if (inode->double_indirect == 0) {
        if (!create) return -1;
        int diblk = sfs_alloc_block(fs);
        if (diblk < 0) return -1;
        inode->double_indirect = (uint32_t)diblk;
        uint8_t z[SFS_BLOCK_SIZE];
        kmemset(z, 0, SFS_BLOCK_SIZE);
        sfs_write_meta(fs, inode->double_indirect, z);
    }

    uint8_t dibuf[SFS_BLOCK_SIZE];
    {
        err_t e = sfs_read_data(fs, inode->double_indirect, dibuf);
        if (e && !create) return -1;
    }

    uint32_t* dptrs = (uint32_t*)dibuf;
    uint32_t di_major = dindirect_idx / SFS_INDIRECT_PTRS;
    uint32_t di_minor = dindirect_idx % SFS_INDIRECT_PTRS;

    if (dptrs[di_major] == 0) {
        if (!create) return -1;
        int ib = sfs_alloc_block(fs);
        if (ib < 0) return -1;
        dptrs[di_major] = (uint32_t)ib;
        sfs_write_meta(fs, inode->double_indirect, dibuf);
        uint8_t z[SFS_BLOCK_SIZE];
        kmemset(z, 0, SFS_BLOCK_SIZE);
        sfs_write_meta(fs, dptrs[di_major], z);
    }

    uint8_t ibuf2[SFS_BLOCK_SIZE];
    {
        err_t e = sfs_read_data(fs, dptrs[di_major], ibuf2);
        if (e && !create) return -1;
    }

    uint32_t* iptrs = (uint32_t*)ibuf2;
    if (iptrs[di_minor] == 0) {
        if (!create) return -1;
        int b = sfs_alloc_block(fs);
        if (b < 0) return -1;
        iptrs[di_minor] = (uint32_t)b;
        sfs_write_meta(fs, dptrs[di_major], ibuf2);
    }
    return (int)iptrs[di_minor];
}

static err_t sfs_read_block_data(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t target_block, uint32_t file_off, void* buf, uint32_t count) {
    uint8_t tmp[SFS_BLOCK_SIZE];
    err_t e = sfs_read_data(fs, target_block, tmp);
    if (e) return e;
    uint32_t copy = count;
    if (file_off + copy > inode->size) copy = inode->size - file_off;
    kmemcpy(buf, tmp + file_off, copy);
    return ERR_OK;
}

static err_t sfs_write_block_data(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t target_block, uint32_t file_off, const void* buf, uint32_t count) {
    uint8_t tmp[SFS_BLOCK_SIZE];
    if (count < SFS_BLOCK_SIZE) {
        err_t e = sfs_read_data(fs, target_block, tmp);
        if (e) kmemset(tmp, 0, SFS_BLOCK_SIZE);
    }
    kmemcpy(tmp + file_off, buf, count);
    return sfs_write_meta(fs, target_block, tmp);
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

/* Helper: load a temporary sfs_file_t from node->inode when private_data is NULL */
static int sfs_temp_load(vfs_node_t* node, sfs_file_t* tmp) {
    sfs_fs_t* fs = (sfs_fs_t*)((uint64_t)node->fs - __builtin_offsetof(sfs_fs_t, vfs_fs));
    kmemset(tmp, 0, sizeof(*tmp));
    tmp->fs = fs;
    tmp->inum = (int)node->inode;
    return sfs_read_inode(fs, tmp->inum, &tmp->inode);
}

static void sfs_file_destructor(void* p) { if (p) kfree(p); }

static int sfs_vfs_open(vfs_node_t* node) {
    if (!node) return -1;
    if (!node->private_data && node->inode) {
        sfs_file_t* f = kmalloc(sizeof(sfs_file_t));
        if (!f) return -1;
        f->fs = (sfs_fs_t*)((uint64_t)node->fs - __builtin_offsetof(sfs_fs_t, vfs_fs));
        f->inum = (int)node->inode;
        f->offset = 0;
        sfs_read_inode(f->fs, f->inum, &f->inode);
        node->private_data = f;
        node->destructor = sfs_file_destructor;
    }
    return 0;
}

static int sfs_vfs_close(vfs_node_t* node) {
    (void)node;
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
        if (phys < 0) {
            kmemset((uint8_t*)buf + done, 0, chunk);
        } else {
            if (sfs_read_block_data(f->fs, &f->inode, (uint32_t)phys, block_off, (uint8_t*)buf + done, chunk))
                break;
        }
        done += chunk;
    }
    if (done > 0) {
        f->inode.atime = (uint32_t)hal_timer_get_ticks();
        sfs_write_inode(f->fs, f->inum, &f->inode);
    }
    return (int64_t)done;
}

static int64_t sfs_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) return -1;

    sfs_begin_op(f->fs);
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
            sfs_end_op(f->fs);
            return (int64_t)done;
        }

        if (sfs_write_block_data(f->fs, &f->inode, (uint32_t)phys, block_off, (const uint8_t*)buf + done, chunk)) {
            f->inode.mtime = (uint32_t)hal_timer_get_ticks();
            sfs_write_inode(f->fs, f->inum, &f->inode);
            sfs_end_op(f->fs);
            return (int64_t)done;
        }
        done += chunk;
    }

    uint64_t new_size = offset + done;
    if (new_size > f->inode.size) {
        f->inode.size = (uint32_t)new_size;
    }
    f->inode.mtime = (uint32_t)hal_timer_get_ticks();
    sfs_write_inode(f->fs, f->inum, &f->inode);
    node->size = f->inode.size;
    sfs_end_op(f->fs);
    return (int64_t)done;
}

static int sfs_vfs_readdir(vfs_node_t* node, uint32_t index, vfs_node_t** out) {
    if (!node) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
    if (f->inode.type != SFS_TYPE_DIR) return -1;

    sfs_read_inode(f->fs, f->inum, &f->inode);
    uint32_t max_entries = f->inode.size / sizeof(sfs_dirent_t);
    if (index >= max_entries) return -1;

    uint32_t byte_off = index * sizeof(sfs_dirent_t);
    uint32_t entry_block = byte_off / SFS_BLOCK_SIZE;
    uint32_t entry_off = byte_off % SFS_BLOCK_SIZE;
    uint32_t file_block_num = entry_block;

    uint8_t tmp[2 * SFS_BLOCK_SIZE];
    int phys = sfs_inode_get_block(f->fs, &f->inode, file_block_num, 0);
    if (phys < 0) return -1;
    err_t e = sfs_read_data(f->fs, (uint32_t)phys, tmp);
    if (e) return -1;

    sfs_dirent_t* de;
    if (entry_off + sizeof(sfs_dirent_t) > SFS_BLOCK_SIZE) {
        /* Cross-block dirent — read next block too */
        int next_phys = sfs_inode_get_block(f->fs, &f->inode, file_block_num + 1, 0);
        if (next_phys < 0) return -1;
        e = sfs_read_data(f->fs, (uint32_t)next_phys, tmp + SFS_BLOCK_SIZE);
        if (e) return -1;
        de = (sfs_dirent_t*)(tmp + entry_off);
    } else {
        de = (sfs_dirent_t*)(tmp + entry_off);
    }

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
        child->destructor = sfs_file_destructor;
    }

    *out = child;
    return 0;
}

static int sfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !st) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
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

static void sfs_free_indirect_chain(sfs_fs_t* fs, uint32_t indirect_block) {
    if (!indirect_block) return;
    uint8_t ibuf[SFS_BLOCK_SIZE];
    if (sfs_read_data(fs, indirect_block, ibuf) != ERR_OK) return;
    uint32_t* ptrs = (uint32_t*)ibuf;
    for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
        if (ptrs[i]) sfs_free_block(fs, ptrs[i]);
    }
    sfs_free_block(fs, indirect_block);
}

static int sfs_vfs_truncate(vfs_node_t* node, uint64_t size) {
    if (!node) return -1;
    int heap_alloced = 0;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        f = kmalloc(sizeof(sfs_file_t));
        if (!f) return -1;
        if (sfs_temp_load(node, f) != ERR_OK) { kfree(f); return -1; }
        node->private_data = f;
        node->destructor = sfs_file_destructor;
        heap_alloced = 1;
    }
    if (size >= f->inode.size) {
        if (heap_alloced) { kfree(f); node->private_data = NULL; node->destructor = NULL; }
        return 0;
    }

    sfs_begin_op(f->fs);
    uint32_t new_blocks = (uint32_t)((size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE);
    uint32_t old_blocks = (uint32_t)((f->inode.size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE);

    /* Free direct blocks */
    for (uint32_t b = new_blocks; b < old_blocks && b < SFS_DIRECT_BLOCKS; b++) {
        if (f->inode.direct[b]) {
            sfs_free_block(f->fs, f->inode.direct[b]);
            f->inode.direct[b] = 0;
        }
    }

    /* Free singly-indirect blocks (file_block 12-139) */
    if (f->inode.indirect && new_blocks <= SFS_INDIRECT_START) {
        sfs_free_indirect_chain(f->fs, f->inode.indirect);
        f->inode.indirect = 0;
    } else if (f->inode.indirect) {
        uint32_t kept = 0;
        uint32_t freed = 0;
        uint8_t ibuf[SFS_BLOCK_SIZE];
        sfs_read_data(f->fs, f->inode.indirect, ibuf);
        uint32_t* ptrs = (uint32_t*)ibuf;
        for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
            uint32_t blk = SFS_INDIRECT_START + i;
            if (ptrs[i]) {
                if (blk < new_blocks) {
                    kept++;
                } else {
                    sfs_free_block(f->fs, ptrs[i]);
                    ptrs[i] = 0;
                    freed++;
                }
            }
        }
        if (freed > 0) {
            if (kept == 0) {
                sfs_free_block(f->fs, f->inode.indirect);
                f->inode.indirect = 0;
            } else {
                sfs_write_meta(f->fs, f->inode.indirect, ibuf);
            }
        }
    }

    /* Free doubly-indirect blocks (file_block 140-16523) */
    if (f->inode.double_indirect && new_blocks <= SFS_DINDIRECT_START) {
        uint8_t dibuf[SFS_BLOCK_SIZE];
        if (sfs_read_data(f->fs, f->inode.double_indirect, dibuf) == ERR_OK) {
            uint32_t* dptrs = (uint32_t*)dibuf;
            for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                if (dptrs[i]) {
                    sfs_free_indirect_chain(f->fs, dptrs[i]);
                }
            }
        }
        sfs_free_block(f->fs, f->inode.double_indirect);
        f->inode.double_indirect = 0;
    } else if (f->inode.double_indirect) {
        uint8_t dibuf[SFS_BLOCK_SIZE];
        if (sfs_read_data(f->fs, f->inode.double_indirect, dibuf) != ERR_OK) goto done;
        uint32_t* dptrs = (uint32_t*)dibuf;
        uint32_t dind_changed = 0;
        for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
            if (!dptrs[i]) continue;
            uint32_t di_block_start = SFS_DINDIRECT_START + i * SFS_INDIRECT_PTRS;
            uint32_t di_block_end = di_block_start + SFS_INDIRECT_PTRS;
            if (new_blocks >= di_block_end) {
                continue;
            }
            if (new_blocks <= di_block_start) {
                sfs_free_indirect_chain(f->fs, dptrs[i]);
                dptrs[i] = 0;
                dind_changed = 1;
            } else {
                uint32_t local_start = new_blocks - di_block_start;
                uint32_t kept_local = 0;
                uint8_t ibuf[SFS_BLOCK_SIZE];
                if (sfs_read_data(f->fs, dptrs[i], ibuf) != ERR_OK) continue;
                uint32_t* iptrs = (uint32_t*)ibuf;
                uint32_t local_freed = 0;
                for (uint32_t j = local_start; j < SFS_INDIRECT_PTRS; j++) {
                    if (iptrs[j]) {
                        sfs_free_block(f->fs, iptrs[j]);
                        iptrs[j] = 0;
                        local_freed++;
                    }
                }
                if (local_freed > 0) {
                    for (uint32_t j = 0; j < local_start; j++) {
                        if (iptrs[j]) kept_local++;
                    }
                    if (kept_local == 0) {
                        sfs_free_block(f->fs, dptrs[i]);
                        dptrs[i] = 0;
                    } else {
                        sfs_write_meta(f->fs, dptrs[i], ibuf);
                    }
                    dind_changed = 1;
                }
            }
        }
        if (dind_changed) {
            int all_zero = 1;
            for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                if (dptrs[i]) { all_zero = 0; break; }
            }
            if (all_zero) {
                sfs_free_block(f->fs, f->inode.double_indirect);
                f->inode.double_indirect = 0;
            } else {
                sfs_write_meta(f->fs, f->inode.double_indirect, dibuf);
            }
        }
    }

done:
    f->inode.size = (uint32_t)size;
    f->inode.mtime = (uint32_t)hal_timer_get_ticks();
    sfs_write_inode(f->fs, f->inum, &f->inode);
    node->size = f->inode.size;
    sfs_end_op(f->fs);
    if (heap_alloced) { kfree(f); node->private_data = NULL; node->destructor = NULL; }
    return 0;
}

static int sfs_vfs_chmod(vfs_node_t* node, uint32_t mode) {
    if (!node) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
    f->inode.mode = mode & 0xFFFF;
    sfs_write_inode(f->fs, f->inum, &f->inode);
    return 0;
}

static int sfs_vfs_lock(vfs_node_t* node) {
    if (!node) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
    f->inode.flags |= SFS_INODE_FLAG_LOCKED;
    sfs_write_inode(f->fs, f->inum, &f->inode);
    return 0;
}

static int sfs_vfs_unlock(vfs_node_t* node) {
    if (!node) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
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

    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t byte_off = i * sizeof(sfs_dirent_t);
        uint32_t bi = byte_off / SFS_BLOCK_SIZE;
        uint32_t off = byte_off % SFS_BLOCK_SIZE;

        uint8_t tmp[2 * SFS_BLOCK_SIZE];
        if (off + sizeof(sfs_dirent_t) > SFS_BLOCK_SIZE) {
            int phys0 = sfs_inode_get_block(fs, &dir_inode, bi, 0);
            if (phys0 < 0) return -1;
            int phys1 = sfs_inode_get_block(fs, &dir_inode, bi + 1, 0);
            if (phys1 < 0) return -1;
            if (sfs_read_data(fs, (uint32_t)phys0, tmp) != ERR_OK) return -1;
            if (sfs_read_data(fs, (uint32_t)phys1, tmp + SFS_BLOCK_SIZE) != ERR_OK) return -1;
        } else {
            int phys = sfs_inode_get_block(fs, &dir_inode, bi, 0);
            if (phys < 0) return -1;
            if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) return -1;
        }
        sfs_dirent_t* de = (sfs_dirent_t*)(tmp + off);
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

    /* Find the entry to remove */
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t byte_off = i * sizeof(sfs_dirent_t);
        uint32_t bi = byte_off / SFS_BLOCK_SIZE;
        uint32_t off = byte_off % SFS_BLOCK_SIZE;

        uint8_t tmp[2 * SFS_BLOCK_SIZE];
        int phys = sfs_inode_get_block(fs, &dir_inode, bi, 0);
        if (phys < 0) return ERR_NOENT;
        if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) return ERR_IO;
        if (off + sizeof(sfs_dirent_t) > SFS_BLOCK_SIZE) {
            int next_phys = sfs_inode_get_block(fs, &dir_inode, bi + 1, 0);
            if (next_phys < 0) return ERR_NOENT;
            if (sfs_read_data(fs, (uint32_t)next_phys, tmp + SFS_BLOCK_SIZE) != ERR_OK) return ERR_IO;
        }
        sfs_dirent_t* de = (sfs_dirent_t*)(tmp + off);
        if (de->inode != 0 && kstrcmp(de->name, name) == 0) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) return ERR_NOENT;

    /* Compact: shift all subsequent entries back by one */
    /* We pack entries tightly without padding, so the compact operation
       shifts raw bytes, handling cross-block boundaries naturally via
       read-modify-write of each affected block. */
    uint32_t src_start = (uint32_t)(found_idx + 1) * sizeof(sfs_dirent_t);
    uint32_t dst_start = (uint32_t)found_idx * sizeof(sfs_dirent_t);
    uint32_t move_size = (max_entries - (uint32_t)found_idx - 1) * sizeof(sfs_dirent_t);

    if (move_size > 0) {
        uint32_t src_byte = src_start;
        uint32_t dst_byte = dst_start;
        uint32_t remaining = move_size;

        while (remaining > 0) {
            uint32_t src_block = src_byte / SFS_BLOCK_SIZE;
            uint32_t src_off = src_byte % SFS_BLOCK_SIZE;
            uint32_t dst_block = dst_byte / SFS_BLOCK_SIZE;
            uint32_t dst_off = dst_byte % SFS_BLOCK_SIZE;

            uint32_t chunk = SFS_BLOCK_SIZE - src_off;
            if (chunk > remaining) chunk = remaining;

            uint8_t sbuf[SFS_BLOCK_SIZE];
            int sphys = sfs_inode_get_block(fs, &dir_inode, src_block, 0);
            if (sphys < 0) break;
            if (sfs_read_data(fs, (uint32_t)sphys, sbuf)) break;

            uint8_t dbuf[SFS_BLOCK_SIZE];
            int dphys = sfs_inode_get_block(fs, &dir_inode, dst_block, 0);
            if (dphys < 0) break;
            if (sfs_read_data(fs, (uint32_t)dphys, dbuf)) break;

            if (sphys == dphys) {
                /* Same src and dst block — use forward copy for left-shift */
                if (src_off > dst_off) {
                    for (uint32_t _k = 0; _k < chunk; _k++)
                        dbuf[dst_off + _k] = sbuf[src_off + _k];
                } else {
                    kmemcpy(dbuf + dst_off, sbuf + src_off, chunk);
                }
                if (sfs_write_meta(fs, (uint32_t)dphys, dbuf)) break;
            } else {
                /* Cross-block copy */
                kmemcpy(dbuf + dst_off, sbuf + src_off, chunk);
                if (sfs_write_meta(fs, (uint32_t)dphys, dbuf)) break;
                kmemset(sbuf + src_off, 0, chunk);
                if (sfs_write_meta(fs, (uint32_t)sphys, sbuf)) break;
            }

            src_byte += chunk;
            dst_byte += chunk;
            remaining -= chunk;
        }
    }

    /* Clear the last (now-orphaned) dirent slot by zeroing its bytes */
    uint32_t last_byte = max_entries * sizeof(sfs_dirent_t) - sizeof(sfs_dirent_t);
    uint32_t last_block = last_byte / SFS_BLOCK_SIZE;
    uint32_t last_off = last_byte % SFS_BLOCK_SIZE;
    {
        uint8_t zbuf[SFS_BLOCK_SIZE];
        int zphys = sfs_inode_get_block(fs, &dir_inode, last_block, 0);
        if (zphys >= 0 && sfs_read_data(fs, (uint32_t)zphys, zbuf) == ERR_OK) {
            uint32_t clear_sz = sizeof(sfs_dirent_t);
            if (last_off + clear_sz > SFS_BLOCK_SIZE)
                clear_sz = SFS_BLOCK_SIZE - last_off;
            kmemset(zbuf + last_off, 0, clear_sz);
            sfs_write_meta(fs, (uint32_t)zphys, zbuf);
        }
    }

    uint32_t new_size = dir_inode.size - sizeof(sfs_dirent_t);
    dir_inode.size = new_size;
    sfs_write_inode(fs, dir_inum, &dir_inode);

    return ERR_OK;
}

static int sfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir) {
    if (!dir || !name) return -1;
    sfs_file_t dir_local;
    sfs_file_t* f = (sfs_file_t*)dir->private_data;
    if (!f) {
        if (sfs_temp_load(dir, &dir_local) != ERR_OK) return -1;
        f = &dir_local;
    }
    if (f->inode.type != SFS_TYPE_DIR) { kprintf("[CREATE] not a dir\n"); return -1; }

    if (sfs_lookup(f->fs, f->inum, name) >= 0) { kprintf("[CREATE] exists\n"); return -1; }
    sfs_begin_op(f->fs);

    int inum = sfs_alloc_inode(f->fs);
    if (inum < 0) { kprintf("[CREATE] alloc_inode failed\n"); return -1; }

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
        sfs_end_op(f->fs);
        return -1;
    }

    sfs_end_op(f->fs);
    return inum;
}

static int sfs_vfs_unlink(vfs_node_t* dir, const char* name) {
    if (!dir || !name) return -1;
    sfs_file_t dir_local;
    sfs_file_t* f = (sfs_file_t*)dir->private_data;
    if (!f) {
        if (sfs_temp_load(dir, &dir_local) != ERR_OK) return -1;
        f = &dir_local;
    }

    int inum = sfs_lookup(f->fs, f->inum, name);
    if (inum < 0) return -1;
    sfs_begin_op(f->fs);

    sfs_inode_t inode;
    sfs_read_inode(f->fs, inum, &inode);

    err_t e = sfs_remove_dirent(f->fs, f->inum, name);
    if (e) { sfs_end_op(f->fs); return -1; }

    if (inode.nlink > 1) {
        inode.nlink--;
        sfs_write_inode(f->fs, inum, &inode);
        sfs_end_op(f->fs);
        return 0;
    }

    /* Last link: free blocks and inode */
    for (int i = 0; i < SFS_DIRECT_BLOCKS; i++) {
        if (inode.direct[i])
            sfs_free_block(f->fs, inode.direct[i]);
    }
    if (inode.indirect) {
        sfs_free_indirect_chain(f->fs, inode.indirect);
    }
    if (inode.double_indirect) {
        uint8_t dibuf[SFS_BLOCK_SIZE];
        if (sfs_read_data(f->fs, inode.double_indirect, dibuf) == ERR_OK) {
            uint32_t* dptrs = (uint32_t*)dibuf;
            for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                if (dptrs[i]) sfs_free_indirect_chain(f->fs, dptrs[i]);
            }
        }
        sfs_free_block(f->fs, inode.double_indirect);
    }

    sfs_free_inode(f->fs, inum);
    sfs_end_op(f->fs);
    return 0;
}

/* Remove a dirent by inum; returns 0 if found and removed, -1 if not found */
static int sfs_remove_dirent_by_inum(sfs_fs_t* fs, int dir_inum, int target_inum) {
    sfs_inode_t dir_inode;
    if (sfs_read_inode(fs, dir_inum, &dir_inode) != ERR_OK) return -1;
    uint32_t max_entries = dir_inode.size / sizeof(sfs_dirent_t);
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t byte_off = i * sizeof(sfs_dirent_t);
        uint32_t bi = byte_off / SFS_BLOCK_SIZE;
        uint32_t off = byte_off % SFS_BLOCK_SIZE;

        uint8_t tmp[2 * SFS_BLOCK_SIZE];
        int phys = sfs_inode_get_block(fs, &dir_inode, bi, 0);
        if (phys < 0) return -1;
        if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) return -1;
        if (off + sizeof(sfs_dirent_t) > SFS_BLOCK_SIZE) {
            int next_phys = sfs_inode_get_block(fs, &dir_inode, bi + 1, 0);
            if (next_phys < 0) return -1;
            if (sfs_read_data(fs, (uint32_t)next_phys, tmp + SFS_BLOCK_SIZE) != ERR_OK) return -1;
        }
        sfs_dirent_t* de = (sfs_dirent_t*)(tmp + off);
        if (de->inode == (uint32_t)target_inum) {
            de->inode = 0;
            sfs_write_meta(fs, (uint32_t)phys, tmp);
            return 0;
        }
    }
    return -1;
}

static int sfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                          vfs_node_t* new_dir, const char* new_name) {
    if (!old_dir || !old_name || !new_dir || !new_name) return -1;
    sfs_file_t old_local, new_local;
    sfs_file_t* old_f = (sfs_file_t*)old_dir->private_data;
    if (!old_f) {
        if (sfs_temp_load(old_dir, &old_local) != ERR_OK) return -1;
        old_f = &old_local;
    }
    sfs_file_t* new_f = (sfs_file_t*)new_dir->private_data;
    if (!new_f) {
        if (sfs_temp_load(new_dir, &new_local) != ERR_OK) return -1;
        new_f = &new_local;
    }

    int inum = sfs_lookup(old_f->fs, old_f->inum, old_name);
    if (inum < 0) return -1;

    sfs_begin_op(old_f->fs);

    /* If target exists, atomically replace it */
    int target_inum = sfs_lookup(new_f->fs, new_f->inum, new_name);
    if (target_inum >= 0) {
        if (target_inum == inum) {
            sfs_end_op(old_f->fs);
            return 0;
        }
        if (sfs_remove_dirent_by_inum(old_f->fs, old_f->inum, target_inum) == 0) {
            sfs_free_inode(old_f->fs, target_inum);
        }
    }

    if (sfs_add_dirent(new_f->fs, new_f->inum, new_name, inum) != ERR_OK) {
        sfs_end_op(old_f->fs);
        return -1;
    }
    /* Remove old entry only if old and new dirs are different */
    if (old_f->inum != new_f->inum || kstrcmp(old_name, new_name) != 0)
        sfs_remove_dirent(old_f->fs, old_f->inum, old_name);
    sfs_end_op(old_f->fs);
    return 0;
}

static int sfs_vfs_symlink(vfs_node_t* dir, const char* name, const char* target) {
    if (!dir || !name || !target) return -1;
    sfs_file_t dir_local;
    sfs_file_t* dir_f = (sfs_file_t*)dir->private_data;
    if (!dir_f) {
        if (sfs_temp_load(dir, &dir_local) != ERR_OK) return -1;
        dir_f = &dir_local;
    }

    if (sfs_lookup(dir_f->fs, dir_f->inum, name) >= 0) return -1;

    sfs_begin_op(dir_f->fs);

    int inum = sfs_alloc_inode(dir_f->fs);
    if (inum < 0) { sfs_end_op(dir_f->fs); return -1; }

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
        sfs_end_op(dir_f->fs);
        return -1;
    }
    inode = tmp_f.inode;

    sfs_write_inode(dir_f->fs, inum, &inode);

    if (sfs_add_dirent(dir_f->fs, dir_f->inum, name, inum) != ERR_OK) {
        sfs_free_inode(dir_f->fs, inum);
        sfs_end_op(dir_f->fs);
        return -1;
    }

    sfs_end_op(dir_f->fs);
    return 0;
}

static int sfs_vfs_readlink(vfs_node_t* node, char* buf, uint64_t size) {
    if (!node || !buf) return -1;
    sfs_file_t local_f;
    sfs_file_t* f = (sfs_file_t*)node->private_data;
    if (!f) {
        if (sfs_temp_load(node, &local_f) != ERR_OK) return -1;
        f = &local_f;
    }
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
    sfs_file_t dir_local, tgt_local;
    sfs_file_t* dir_f = (sfs_file_t*)dir->private_data;
    if (!dir_f) {
        if (sfs_temp_load(dir, &dir_local) != ERR_OK) return -1;
        dir_f = &dir_local;
    }
    sfs_file_t* tgt_f = (sfs_file_t*)target->private_data;
    if (!tgt_f) {
        if (sfs_temp_load(target, &tgt_local) != ERR_OK) return -1;
        tgt_f = &tgt_local;
    }

    if (sfs_lookup(dir_f->fs, dir_f->inum, name) >= 0) return -1;

    sfs_begin_op(dir_f->fs);

    int inum = tgt_f->inum;
    if (sfs_add_dirent(dir_f->fs, dir_f->inum, name, inum) != ERR_OK) {
        sfs_end_op(dir_f->fs);
        return -1;
    }

    sfs_inode_t inode;
    sfs_read_inode(tgt_f->fs, inum, &inode);
    inode.nlink++;
    sfs_write_inode(tgt_f->fs, inum, &inode);
    sfs_end_op(dir_f->fs);
    return 0;
}

err_t sfs_format(block_dev_t* bdev) {
    /* Reserve journal blocks at the end of the device */
    uint32_t journal_start = (uint32_t)bdev->block_count - JOURNAL_BLOCKS;
    uint64_t total_blocks = journal_start;
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

    /* Initialise journal */
    journal_init(bdev, journal_start);
    sb.root_inode = 0;
    sfs_sb_finalize(&sb);

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

    block_sync_dev(bdev);
    return ERR_OK;
}

err_t sfs_mount(block_dev_t* bdev) {
    sfs_fs_t* fs = kmalloc(sizeof(sfs_fs_t));
    if (!fs) return ERR_NOMEM;
    kmemset(fs, 0, sizeof(sfs_fs_t));

    fs->bdev = bdev;
    fs->journal_start = (uint32_t)bdev->block_count - JOURNAL_BLOCKS;
    fs->journal_active = 1;
    mutex_init(&fs->bmap_lock);

    /* Recover journal before reading superblock */
    int journal_dirty = 0;
    journal_recover(bdev, fs->journal_start, &journal_dirty);

    err_t e = block_read(bdev, 0, 1, &fs->sb);
    if (e) { kfree(fs); return e; }

    /* Initialise runtime block tracker from on-disk bitmap */
    sfs_init_block_owner(fs);

    /* If journal was dirty, auto-run fsck to catch any lingering inconsistencies */
    if (journal_dirty) {
        kprintf("[SFS] Journal was dirty, running fsck -r...\n");
        sfs_fsck(bdev, 1);
        block_read(bdev, 0, 1, &fs->sb);
        sfs_init_block_owner(fs);
    }

    if (fs->sb.magic != SFS_MAGIC || !sfs_sb_verify(&fs->sb)) {
        kprintf("[SFS] Bad magic 0x%x or bad checksum, need to format\n", fs->sb.magic);
        if (fs->block_owner) kfree(fs->block_owner);
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
    fs->root_node.destructor = sfs_file_destructor;

    vfs_register_fs(&fs->vfs_fs);

    kprintf("[SFS] Mounted on '%s' (%u blocks, %u inodes)\n",
            bdev->name, fs->sb.total_blocks, fs->sb.total_inodes);
    return ERR_OK;
}

int sfs_get_block_size(void) { return SFS_BLOCK_SIZE; }
