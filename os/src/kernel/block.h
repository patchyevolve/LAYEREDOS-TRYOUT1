#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"

#define BLOCK_SIZE  512
#define BLOCK_SHIFT 9

typedef struct block_dev block_dev_t;

typedef err_t (*block_read_t)(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf);
typedef err_t (*block_write_t)(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf);

typedef struct block_dev {
    char          name[16];
    uint64_t      block_count;
    uint32_t      block_size;
    block_read_t  read;
    block_write_t write;
    void*         private_data;
} block_dev_t;

err_t block_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf);
err_t block_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf);
err_t block_sync(void);
err_t block_sync_dev(block_dev_t* dev);
void block_cache_stats(void);
int   block_register(block_dev_t* dev);
int   block_count(void);
block_dev_t* block_get(int index);
block_dev_t* block_find(const char* name);

#endif
