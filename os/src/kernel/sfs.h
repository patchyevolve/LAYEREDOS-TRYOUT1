#ifndef SFS_H
#define SFS_H

#include "types.h"
#include "vfs.h"
#include "block.h"
#include "sync.h"

#define SFS_MAGIC        0x53465301
#define SFS_BLOCK_SIZE   512
#define SFS_MAX_INODES   1024
#define SFS_NAME_MAX     28
#define SFS_DIRECT_BLOCKS 12

#define SFS_TYPE_FREE    0
#define SFS_TYPE_FILE    1
#define SFS_TYPE_DIR     2
#define SFS_TYPE_SYMLINK 3

#define SFS_INODE_FLAG_LOCKED 1

#define SFS_INODES_PER_BLOCK  (SFS_BLOCK_SIZE / sizeof(sfs_inode_t))
#define SFS_DIRENTS_PER_BLOCK (SFS_BLOCK_SIZE / sizeof(sfs_dirent_t))
#define SFS_INDIRECT_PTRS     (SFS_BLOCK_SIZE / 4)
#define SFS_DINDIRECT_PTRS    (SFS_INDIRECT_PTRS * SFS_INDIRECT_PTRS)
#define SFS_INDIRECT_START    SFS_DIRECT_BLOCKS
#define SFS_DINDIRECT_START   (SFS_INDIRECT_START + SFS_INDIRECT_PTRS)

typedef struct sfs_superblock {
    uint32_t magic;
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t inode_bmap_start;
    uint32_t block_bmap_start;
    uint32_t inode_table_start;
    uint32_t data_start;
    uint32_t root_inode;
    uint32_t checksum;
    uint8_t  pad[476];
} __attribute__((packed)) sfs_superblock_t;

typedef struct sfs_inode {
    uint16_t type;
    uint16_t mode;
    uint32_t size;
    uint32_t direct[SFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t nlink;
    uint32_t flags;
} __attribute__((packed)) sfs_inode_t;

typedef struct sfs_dirent {
    uint32_t inode;
    char     name[28];
} __attribute__((packed)) sfs_dirent_t;

typedef struct sfs_fs {
    block_dev_t*  bdev;
    sfs_superblock_t sb;
    vfs_fs_t      vfs_fs;
    vfs_node_t    root_node;
    mutex_t       bmap_lock;
    uint32_t      journal_start;
    int           journal_seq;
    int           journal_active;
    int           journal_nest;
    uint8_t*      block_owner;      // runtime block tracker (idx=block-data_start, 0=free, 1=claimed)
    uint32_t      block_owner_count;
} sfs_fs_t;

err_t sfs_mount(block_dev_t* bdev);
err_t sfs_format(block_dev_t* bdev);
int   sfs_get_block_size(void);
err_t sfs_read_inode(sfs_fs_t* fs, int inum, sfs_inode_t* inode);
err_t sfs_write_inode(sfs_fs_t* fs, int inum, const sfs_inode_t* inode);
int   sfs_inode_get_block(sfs_fs_t* fs, sfs_inode_t* inode, uint32_t file_block, int create);
err_t sfs_read_data(sfs_fs_t* fs, uint32_t block, void* buf);

#endif
