#include "kernel.h"
#include "sfs.h"
#include "fsck.h"
#include "block.h"
#include "journal.h"
#include "pmm.h"
#include "kmalloc.h"

#define FSCK_PASS 0
#define FSCK_FIX  1

static int fsck_verbose = 1;

#define fsck_printf(fmt, ...) \
    do { if (fsck_verbose) kprintf("[FSCK] " fmt, ##__VA_ARGS__); } while(0)
#define fsck_error(fmt, ...) \
    kprintf("[FSCK] ERROR: " fmt, ##__VA_ARGS__)
#define fsck_fix(fmt, ...) \
    kprintf("[FSCK] FIX: " fmt, ##__VA_ARGS__)

/* Build a should-be-allocated block bitmap from inode scan.
   Returns number of inodes found, or -1 on error. */
static int fsck_scan_inodes(sfs_fs_t* fs, uint8_t* block_used, uint32_t total_data_blocks,
                            uint8_t* inode_used, uint32_t total_inodes) {
    int inode_count = 0;
    uint32_t errors = 0;
    uint32_t data_start = fs->sb.data_start;

    /* Read the inode bitmap to cross-check */
    uint32_t imap_blocks = (total_inodes + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
    uint8_t* imap_disk = (uint8_t*)kmalloc(imap_blocks * SFS_BLOCK_SIZE);
    if (!imap_disk) return -1;
    kmemset(imap_disk, 0, imap_blocks * SFS_BLOCK_SIZE);
    for (uint32_t b = 0; b < imap_blocks; b++)
        block_read(fs->bdev, fs->sb.inode_bmap_start + b, 1, imap_disk + b * SFS_BLOCK_SIZE);

    for (uint32_t inum = 0; inum < total_inodes; inum++) {
        uint32_t byte = inum / 8, bit = inum % 8;
        int bmap_free = !(imap_disk[byte] & (1 << bit));

        sfs_inode_t inode;
        if (sfs_read_inode(fs, (int)inum, &inode) != ERR_OK) {
            fsck_error("Cannot read inode %u\n", inum);
            errors++;
            continue;
        }

        if (inode.type == SFS_TYPE_FREE) {
            if (!bmap_free) {
                fsck_error("Inode %u: type=FREE but bitmap says allocated\n", inum);
                errors++;
            }
            continue;
        }

        if (bmap_free) {
            fsck_printf("Inode %u: type=%u but bitmap says free (stale inode table entry, OK)\n",
                        inum, inode.type);
            continue;
        }
        if (inode.type > SFS_TYPE_SYMLINK) {
            fsck_error("Inode %u: invalid type %u\n", inum, inode.type);
            errors++;
            continue;
        }

        inode_used[inum / 8] |= (1 << (inum % 8));
        inode_count++;

        /* Check direct blocks */
        for (int i = 0; i < SFS_DIRECT_BLOCKS; i++) {
            if (!inode.direct[i]) continue;
            uint32_t blk = inode.direct[i];
            if (blk < data_start || blk >= data_start + total_data_blocks) {
                fsck_error("Inode %u: direct[%d] block %u out of range\n", inum, i, blk);
                errors++;
                continue;
            }
            uint32_t idx = blk - data_start;
            if (block_used[idx / 8] & (1 << (idx % 8))) {
                fsck_error("Inode %u: direct[%d] block %u double-allocated\n", inum, i, blk);
                errors++;
            }
            block_used[idx / 8] |= (1 << (idx % 8));
        }

        /* Check indirect block */
        if (inode.indirect) {
            uint32_t blk = inode.indirect;
            if (blk < data_start || blk >= data_start + total_data_blocks) {
                fsck_error("Inode %u: indirect block %u out of range\n", inum, blk);
                errors++;
            } else {
                uint32_t idx = blk - data_start;
                block_used[idx / 8] |= (1 << (idx % 8));
            }

            uint8_t ibuf[SFS_BLOCK_SIZE];
            if (sfs_read_data(fs, inode.indirect, ibuf) == ERR_OK) {
                uint32_t* ptrs = (uint32_t*)ibuf;
                for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                    if (!ptrs[i]) continue;
                    uint32_t dblk = ptrs[i];
                    if (dblk < data_start || dblk >= data_start + total_data_blocks) {
                        fsck_error("Inode %u: indirect[%u] block %u out of range\n", inum, i, dblk);
                        errors++;
                        continue;
                    }
                    uint32_t didx = dblk - data_start;
                    block_used[didx / 8] |= (1 << (didx % 8));
                }
            }
        }

        /* Check double-indirect block */
        if (inode.double_indirect) {
            uint32_t blk = inode.double_indirect;
            if (blk < data_start || blk >= data_start + total_data_blocks) {
                fsck_error("Inode %u: double_indirect block %u out of range\n", inum, blk);
                errors++;
            } else {
                uint32_t idx = blk - data_start;
                block_used[idx / 8] |= (1 << (idx % 8));
            }

            uint8_t dibuf[SFS_BLOCK_SIZE];
            if (sfs_read_data(fs, inode.double_indirect, dibuf) == ERR_OK) {
                uint32_t* dptrs = (uint32_t*)dibuf;
                for (uint32_t i = 0; i < SFS_INDIRECT_PTRS; i++) {
                    if (!dptrs[i]) continue;
                    uint32_t iblk = dptrs[i];
                    if (iblk < data_start || iblk >= data_start + total_data_blocks) {
                        fsck_error("Inode %u: dindirect[%u] indirect block %u out of range\n", inum, i, iblk);
                        errors++;
                        continue;
                    }
                    uint32_t iidx = iblk - data_start;
                    block_used[iidx / 8] |= (1 << (iidx % 8));

                    uint8_t ibuf[SFS_BLOCK_SIZE];
                    if (sfs_read_data(fs, iblk, ibuf) == ERR_OK) {
                        uint32_t* iptrs = (uint32_t*)ibuf;
                        for (uint32_t j = 0; j < SFS_INDIRECT_PTRS; j++) {
                            if (!iptrs[j]) continue;
                            uint32_t dblk = iptrs[j];
                            if (dblk < data_start || dblk >= data_start + total_data_blocks) {
                                fsck_error("Inode %u: dindirect[%u][%u] block %u out of range\n", inum, i, j, dblk);
                                errors++;
                                continue;
                            }
                            uint32_t didx = dblk - data_start;
                            block_used[didx / 8] |= (1 << (didx % 8));
                        }
                    }
                }
            }
        }
    }

    kfree(imap_disk);
    if (errors) {
        fsck_error("Inode scan completed with %u errors\n", errors);
    } else {
        fsck_printf("Inode scan OK: %d inodes found\n", inode_count);
    }
    return inode_count;
}

/* Compare a should-be-allocated bitmap with the actual on-disk bitmap.
   If repair is set, fix discrepancies. */
static uint32_t fsck_check_bitmap(sfs_fs_t* fs, const char* name,
                                   uint32_t bmap_start, uint32_t total_entries,
                                   uint8_t* should, int repair) {
    uint32_t errors = 0;
    uint32_t bmap_blocks = (total_entries + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);

    for (uint32_t b = 0; b < bmap_blocks; b++) {
        uint8_t disk_buf[SFS_BLOCK_SIZE];
        if (block_read(fs->bdev, bmap_start + b, 1, disk_buf) != ERR_OK) {
            fsck_error("Cannot read %s bitmap block %u\n", name, b);
            errors++;
            continue;
        }

        for (uint32_t i = 0; i < SFS_BLOCK_SIZE * 8; i++) {
            uint32_t entry = b * SFS_BLOCK_SIZE * 8 + i;
            if (entry >= total_entries) break;

            int disk_set = (disk_buf[i / 8] >> (i % 8)) & 1;
            int should_set = (should[entry / 8] >> (entry % 8)) & 1;

            if (disk_set && !should_set) {
                errors++;
                fsck_error("%s bitmap: entry %u allocated but should be free\n", name, entry);
                if (repair)
                    disk_buf[i / 8] &= ~(1 << (i % 8));
            } else if (!disk_set && should_set) {
                errors++;
                fsck_error("%s bitmap: entry %u free but should be allocated\n", name, entry);
                if (repair)
                    disk_buf[i / 8] |= (1 << (i % 8));
            }
        }

        if (errors > 0 && repair)
            block_write(fs->bdev, bmap_start + b, 1, disk_buf);
    }

    if (errors == 0)
        fsck_printf("%s bitmap OK (%u entries)\n", name, total_entries);
    else
        fsck_error("%s bitmap: %u errors%s\n", name, errors,
                   repair ? " (repaired)" : "");
    return errors;
}

/* Verify all directories: check entries point to valid inodes */
static uint32_t fsck_check_dirs(sfs_fs_t* fs, uint8_t* inode_used, uint32_t total_inodes) {
    uint32_t errors = 0;

    for (uint32_t inum = 0; inum < total_inodes; inum++) {
        sfs_inode_t inode;
        if (sfs_read_inode(fs, (int)inum, &inode) != ERR_OK) continue;
        if (inode.type != SFS_TYPE_DIR) continue;

        uint32_t max_entries = inode.size / sizeof(sfs_dirent_t);
        uint8_t tmp[SFS_BLOCK_SIZE];

        for (uint32_t i = 0; i < max_entries; i++) {
            uint32_t bi = i / SFS_DIRENTS_PER_BLOCK;
            uint32_t off = i % SFS_DIRENTS_PER_BLOCK;

            if (off == 0) {
                int phys = sfs_inode_get_block(fs, &inode, bi, 0);
                if (phys < 0) {
                    fsck_error("Dir inode %u: missing block for entry %u\n", inum, i);
                    errors++;
                    break;
                }
                if (sfs_read_data(fs, (uint32_t)phys, tmp) != ERR_OK) {
                    errors++;
                    break;
                }
            }

            sfs_dirent_t* de = (sfs_dirent_t*)tmp + off;
            if (de->inode == 0) {
                continue;
            }
            if (de->inode >= total_inodes) {
                fsck_error("Dir inode %u: entry %u points to out-of-range inode %u\n", inum, i, de->inode);
                errors++;
                continue;
            }
            if (!(inode_used[de->inode / 8] & (1 << (de->inode % 8)))) {
                fsck_error("Dir inode %u: entry '%s' points to free inode %u\n", inum, de->name, de->inode);
                errors++;
                continue;
            }
            sfs_inode_t cinode;
            sfs_read_inode(fs, (int)de->inode, &cinode);
            if (cinode.type == SFS_TYPE_FREE) {
                fsck_error("Dir inode %u: entry '%s' points to type-FREE inode %u\n", inum, de->name, de->inode);
                errors++;
            }
        }
    }

    if (errors == 0)
        fsck_printf("Directory scan OK\n");
    else
        fsck_error("Directory scan: %u errors%s\n", errors, "");
    return errors;
}

err_t sfs_fsck(block_dev_t* bdev, int repair) {
    fsck_printf("Starting filesystem check on '%s' (repair=%s)\n",
                bdev->name, repair ? "yes" : "no");

    /* Read superblock */
    sfs_superblock_t sb;
    if (block_read(bdev, 0, 1, &sb) != ERR_OK) {
        fsck_error("Cannot read superblock\n");
        return ERR_IO;
    }

    if (sb.magic != SFS_MAGIC) {
        fsck_error("Bad magic 0x%x (expected 0x%x)\n", sb.magic, SFS_MAGIC);
        return ERR_INVAL;
    }

    uint32_t stored = sb.checksum;
    sb.checksum = 0;
    uint32_t calc = 0;
    const uint32_t* p = (const uint32_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sfs_superblock_t) / sizeof(uint32_t); i++)
        calc ^= p[i];
    sb.checksum = stored;

    if (calc != stored) {
        fsck_error("Superblock checksum mismatch: stored=0x%x, calculated=0x%x\n", stored, calc);
        if (repair) {
            sfs_superblock_t sb_disk;
            block_read(bdev, 0, 1, &sb_disk);
            sb_disk.checksum = 0;
            sb_disk.checksum = calc;
            block_write(bdev, 0, 1, &sb_disk);
            fsck_fix("Superblock checksum corrected\n");
        }
    } else {
        fsck_printf("Superblock checksum OK\n");
    }

    uint32_t total_blocks = sb.total_blocks;
    uint32_t total_inodes = sb.total_inodes;
    uint32_t data_start = sb.data_start;
    uint32_t total_data_blocks = total_blocks - data_start;

    fsck_printf("Layout: %u blocks, %u inodes, %u data blocks starting at %u\n",
                total_blocks, total_inodes, total_data_blocks, data_start);

    if (total_data_blocks == 0 || total_inodes == 0) {
        fsck_error("Empty filesystem\n");
        return ERR_INVAL;
    }

    /* Replay journal so bitmaps reflect committed state */
    uint32_t journal_start = (uint32_t)bdev->block_count - JOURNAL_BLOCKS;
    int jd = 0;
    journal_recover(bdev, journal_start, &jd);
    fsck_printf("Journal recovered\n");

    /* Create temporary bitmaps */
    uint32_t bmap_bytes = (total_data_blocks + 7) / 8;
    uint32_t imap_bytes = (total_inodes + 7) / 8;
    uint8_t* block_used = (uint8_t*)kmalloc(bmap_bytes);
    uint8_t* inode_used = (uint8_t*)kmalloc(imap_bytes);
    if (!block_used || !inode_used) {
        if (block_used) kfree(block_used);
        if (inode_used) kfree(inode_used);
        return ERR_NOMEM;
    }
    kmemset(block_used, 0, bmap_bytes);
    kmemset(inode_used, 0, imap_bytes);

    /* Set up a temporary fs context for reading */
    sfs_fs_t tmp_fs;
    kmemset(&tmp_fs, 0, sizeof(tmp_fs));
    tmp_fs.bdev = bdev;
    kmemcpy(&tmp_fs.sb, &sb, sizeof(sb));
    tmp_fs.journal_active = 0;
    tmp_fs.journal_start = (uint32_t)bdev->block_count - JOURNAL_BLOCKS;

    /* Pass 1: Scan all inodes */
    uint32_t total_errors = 0;
    int inode_count = fsck_scan_inodes(&tmp_fs, block_used, total_data_blocks, inode_used, total_inodes);
    if (inode_count < 0) {
        kfree(block_used);
        kfree(inode_used);
        return ERR_IO;
    }

    /* Pass 2: Check block bitmap */
    total_errors += fsck_check_bitmap(&tmp_fs, "Block",
                                       sb.block_bmap_start, total_data_blocks,
                                       block_used, repair);

    /* Pass 3: Check inode bitmap */
    total_errors += fsck_check_bitmap(&tmp_fs, "Inode",
                                       sb.inode_bmap_start, total_inodes,
                                       inode_used, repair);

    /* Pass 4: Check directory entries */
    total_errors += fsck_check_dirs(&tmp_fs, inode_used, total_inodes);

    kfree(block_used);
    kfree(inode_used);

    fsck_printf("Check complete: %u errors found%s\n", total_errors,
                total_errors == 0 ? " (filesystem is clean)" :
                repair ? " (some repaired)" : " (use fsck -r to fix)");
    return total_errors == 0 ? ERR_OK : ERR_GENERAL;
}
