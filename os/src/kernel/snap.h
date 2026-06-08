#ifndef SNAP_H
#define SNAP_H

#include "types.h"
#include "block.h"

err_t snapshot_take(block_dev_t* bdev);
err_t snapshot_rollback(block_dev_t* bdev);
int   snapshot_exists(void);

#endif
