#ifndef JOURNAL_H
#define JOURNAL_H

#include "types.h"
#include "block.h"

#define JOURNAL_MAGIC     0x4A524E4C
#define JOURNAL_BLOCKS    256
#define JOURNAL_CHECKSUM_SEED 0xDEADBEEF

#define JENT_DESC  1
#define JENT_DATA  2
#define JENT_COMMIT 3

#define JSB_BLOCK(start)     ((uint64_t)(start))
#define JENT_BLOCK(start, i) ((uint64_t)(start) + 1 + (i))
#define JENT_COUNT           (JOURNAL_BLOCKS - 1)

err_t journal_init(block_dev_t* bdev, uint32_t start);
err_t journal_recover(block_dev_t* bdev, uint32_t start, int* was_dirty);
err_t journal_start_txn(block_dev_t* bdev, uint32_t start, uint32_t* seq);
err_t journal_log(block_dev_t* bdev, uint32_t start, uint32_t seq, uint32_t block, const void* data);
err_t journal_commit(block_dev_t* bdev, uint32_t start, uint32_t seq);
err_t journal_checkpoint(block_dev_t* bdev, uint32_t start);

#endif
