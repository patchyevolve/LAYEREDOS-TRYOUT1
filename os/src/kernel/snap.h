#ifndef SNAP_H
#define SNAP_H

#include "types.h"
#include "block.h"

#define SNAP_MAX     4
#define SNAP_DESC_LEN 32

typedef struct {
    int        used;
    uint32_t   blocks;
    uint64_t   timestamp;
    char       desc[SNAP_DESC_LEN];
} snap_info_t;

err_t snapshot_take(block_dev_t* bdev, const char* desc);
err_t snapshot_rollback(block_dev_t* bdev, int index);
int   snapshot_count(void);
int   snapshot_info(int index, snap_info_t* info);
void  snapshot_free(int index);

#endif
