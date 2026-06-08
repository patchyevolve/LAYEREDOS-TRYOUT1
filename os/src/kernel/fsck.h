#ifndef FSCK_H
#define FSCK_H

#include "types.h"
#include "block.h"

err_t sfs_fsck(block_dev_t* bdev, int repair);

#endif
