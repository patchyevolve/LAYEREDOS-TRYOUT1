#include "kernel.h"
#include "block.h"
#include "swap.h"
#include "journal.h"
#include "snap.h"
#include "pmm.h"

#ifdef STORAGE_SELF_TEST

#define TEST_PASS 0
#define TEST_FAIL 1

/* -----------------------------------------------------------------
 * Fake block device backed by a static buffer (128 KB = 256 blocks)
 * ----------------------------------------------------------------- */
#define TEST_NDISK_BLOCKS 256
static uint8_t test_disk[BLOCK_SIZE * TEST_NDISK_BLOCKS];
static block_dev_t test_bdev;

static err_t test_blk_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    (void)dev;
    if (lba + count > TEST_NDISK_BLOCKS) return ERR_INVAL;
    kmemcpy(buf, test_disk + (size_t)lba * BLOCK_SIZE, (size_t)count * BLOCK_SIZE);
    return ERR_OK;
}

static err_t test_blk_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    (void)dev;
    if (lba + count > TEST_NDISK_BLOCKS) return ERR_INVAL;
    kmemcpy(test_disk + (size_t)lba * BLOCK_SIZE, buf, (size_t)count * BLOCK_SIZE);
    return ERR_OK;
}

static void test_bdev_init(void) {
    kmemset(&test_bdev, 0, sizeof(test_bdev));
    kstrncpy(test_bdev.name, "testblk", sizeof(test_bdev.name) - 1);
    test_bdev.block_count = TEST_NDISK_BLOCKS;
    test_bdev.block_size = BLOCK_SIZE;
    test_bdev.read = test_blk_read;
    test_bdev.write = test_blk_write;
    test_bdev.private_data = NULL;
    kmemset(test_disk, 0, sizeof(test_disk));
}

/* ============================================================
 * Test 1: Block cache basic read/write roundtrip
 *
 * Writes a known pattern through the block cache, reads it
 * back, verifies data integrity. Tests the LRU eviction path
 * by writing more blocks than the cache size.
 * ============================================================ */
static int test_block_cache_basic(void) {
    uint8_t wbuf[BLOCK_SIZE];
    uint8_t rbuf[BLOCK_SIZE];

    /* Write to 65 blocks (64 is cache size, 65 forces eviction) */
    for (uint64_t i = 0; i < 65; i++) {
        kmemset(wbuf, (int)(i & 0xFF), BLOCK_SIZE);
        if (block_write(&test_bdev, i, 1, wbuf) != ERR_OK) {
            kprintf("[TEST] block_cache_basic: FAIL — write failed at block %llu\n", i);
            return TEST_FAIL;
        }
    }

    /* Read back and verify */
    for (uint64_t i = 0; i < 65; i++) {
        if (block_read(&test_bdev, i, 1, rbuf) != ERR_OK) {
            kprintf("[TEST] block_cache_basic: FAIL — read failed at block %llu\n", i);
            return TEST_FAIL;
        }
        for (int j = 0; j < BLOCK_SIZE; j++) {
            if (rbuf[j] != (uint8_t)(i & 0xFF)) {
                kprintf("[TEST] block_cache_basic: FAIL — data mismatch at block %llu byte %d\n", i, j);
                return TEST_FAIL;
            }
        }
    }

    kprintf("[TEST] block_cache_basic: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 2: Block cache write-back persistence
 *
 * Writes data, calls block_sync() to force write-back, then
 * checks the underlying device has the data. Verifies the
 * write-back cache actually flushed.
 * ============================================================ */
static int test_block_cache_writeback(void) {
    uint8_t pattern[BLOCK_SIZE];
    uint8_t raw[BLOCK_SIZE];

    kmemset(pattern, 0xA5, BLOCK_SIZE);

    /* Write through cache */
    if (block_write(&test_bdev, 42, 1, pattern) != ERR_OK) {
        kprintf("[TEST] block_cache_writeback: FAIL — write\n");
        return TEST_FAIL;
    }

    /* Sync — forces dirty blocks to device */
    if (block_sync() != ERR_OK) {
        kprintf("[TEST] block_cache_writeback: FAIL — sync\n");
        return TEST_FAIL;
    }

    /* Read directly from device (not through cache) */
    if (test_bdev.read(&test_bdev, 42, 1, raw) != ERR_OK) {
        kprintf("[TEST] block_cache_writeback: FAIL — raw read\n");
        return TEST_FAIL;
    }

    if (kmemcmp(raw, pattern, BLOCK_SIZE) != 0) {
        kprintf("[TEST] block_cache_writeback: FAIL — data mismatch after sync\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] block_cache_writeback: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 3: Block registration rejects non-512 block_size
 *
 * Verifies block_register() refuses devices whose block_size
 * does not match BLOCK_SIZE.
 * ============================================================ */
static int test_block_register_reject(void) {
    block_dev_t bad;
    kmemset(&bad, 0, sizeof(bad));
    kstrncpy(bad.name, "badblk", sizeof(bad.name) - 1);
    bad.block_count = 100;
    bad.block_size = 1024;
    bad.read = test_blk_read;
    bad.write = test_blk_write;

    int idx = block_register(&bad);
    if (idx >= 0) {
        kprintf("[TEST] block_register_reject: FAIL — accepted non-512 block_size\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] block_register_reject: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 4: Swap alloc/free cycle
 *
 * Allocates a swap slot, verifies used_slots > 0, frees it,
 * verifies used_slots back to 0.
 * ============================================================ */
static int test_swap_alloc_free(void) {
    int used_before = swap_used_slots();
    if (used_before != 0) {
        kprintf("[TEST] swap_alloc_free: FAIL — expected 0 used before alloc, got %d\n", used_before);
        return TEST_FAIL;
    }

    int slot = swap_alloc_slot();
    if (slot < 0) {
        kprintf("[TEST] swap_alloc_free: FAIL — alloc_slot returned %d\n", slot);
        return TEST_FAIL;
    }

    int used_after = swap_used_slots();
    if (used_after != 1) {
        kprintf("[TEST] swap_alloc_free: FAIL — expected 1 used after alloc, got %d\n", used_after);
        return TEST_FAIL;
    }

    swap_free_slot(slot);
    int used_freed = swap_used_slots();
    if (used_freed != 0) {
        kprintf("[TEST] swap_alloc_free: FAIL — expected 0 used after free, got %d\n", used_freed);
        return TEST_FAIL;
    }

    kprintf("[TEST] swap_alloc_free: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 5: Swap out/in roundtrip
 *
 * Writes known data via swap_out, reads it back via swap_in,
 * verifies data matches. Uses a physical page.
 * ============================================================ */
static int test_swap_out_in(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[TEST] swap_out_in: FAIL — pmm_alloc_page\n");
        return TEST_FAIL;
    }
    uint8_t* buf = (uint8_t*)PHYS_TO_VIRT(phys);

    const char marker = 0xDB;
    kmemset(buf, marker, PAGE_SIZE);

    int slot = swap_alloc_slot();
    if (slot < 0) {
        pmm_free_page(phys);
        kprintf("[TEST] swap_out_in: FAIL — alloc_slot\n");
        return TEST_FAIL;
    }

    if (swap_out(slot, phys) != ERR_OK) {
        pmm_free_page(phys);
        swap_free_slot(slot);
        kprintf("[TEST] swap_out_in: FAIL — swap_out\n");
        return TEST_FAIL;
    }

    /* Scrub the page */
    kmemset(buf, 0, PAGE_SIZE);

    if (swap_in(slot, phys) != ERR_OK) {
        pmm_free_page(phys);
        swap_free_slot(slot);
        kprintf("[TEST] swap_out_in: FAIL — swap_in\n");
        return TEST_FAIL;
    }

    /* Verify */
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (buf[i] != (uint8_t)marker) {
            pmm_free_page(phys);
            swap_free_slot(slot);
            kprintf("[TEST] swap_out_in: FAIL — data mismatch at byte %zu\n", i);
            return TEST_FAIL;
        }
    }

    pmm_free_page(phys);
    swap_free_slot(slot);
    kprintf("[TEST] swap_out_in: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 6: Journal init + empty recovery
 *
 * Initialises a journal on the test block device, then
 * recovers it (expecting empty journal, no replay).
 *
 * Note: Journal uses direct bdev->read/bdev->write (bypasses
 * cache), so flush the cache before any journal test.
 * ============================================================ */
static int test_journal_init(void) {
    block_sync_dev(&test_bdev);
    err_t e = journal_init(&test_bdev, 0);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_init: FAIL — init returned %d\n", e);
        return TEST_FAIL;
    }

    int was_dirty = -1;
    e = journal_recover(&test_bdev, 0, &was_dirty);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_init: FAIL — recover returned %d\n", e);
        return TEST_FAIL;
    }
    if (was_dirty != 0) {
        kprintf("[TEST] journal_init: FAIL — expected clean recovery\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] journal_init: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 7: Journal log + commit + recover
 *
 * Logs a data entry to a target block, commits the
 * transaction, then recovers. Verifies the committed data
 * was replayed to the target block.
 * ============================================================ */
static int test_journal_log_commit(void) {
    uint32_t seq;
    uint8_t ref[BLOCK_SIZE];
    uint8_t actual[BLOCK_SIZE];

    block_sync_dev(&test_bdev);
    err_t e = journal_init(&test_bdev, 0);
    if (e != ERR_OK) return TEST_FAIL;

    e = journal_start_txn(&test_bdev, 0, &seq);
    if (e != ERR_OK) return TEST_FAIL;

    /* Prepare reference data */
    kmemset(ref, 0, BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++)
        ref[i] = (uint8_t)(i ^ 0xAA);

    /* Log to block 200 (far from journal area 0..255) */
    e = journal_log(&test_bdev, 0, seq, 200, ref);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_log_commit: FAIL — log returned %d\n", e);
        return TEST_FAIL;
    }

    e = journal_commit(&test_bdev, 0, seq);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_log_commit: FAIL — commit returned %d\n", e);
        return TEST_FAIL;
    }

    /* Scrub the target block on disk to verify recovery replays it */
    kmemset(actual, 0, BLOCK_SIZE);
    test_bdev.write(&test_bdev, 200, 1, actual);

    /* Recover — should replay block 200 */
    int was_dirty = -1;
    e = journal_recover(&test_bdev, 0, &was_dirty);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_log_commit: FAIL — recover returned %d\n", e);
        return TEST_FAIL;
    }
    if (was_dirty != 1) {
        kprintf("[TEST] journal_log_commit: FAIL — expected dirty recovery\n");
        return TEST_FAIL;
    }

    /* Read back replayed block */
    test_bdev.read(&test_bdev, 200, 1, actual);

    /* Journal data entries store 496 bytes (BLOCK_SIZE - 16 for headers/checksum).
     * Verify the restored portion matches, and trailing bytes remain zero. */
    if (kmemcmp(actual, ref, 496) != 0) {
        kprintf("[TEST] journal_log_commit: FAIL — data mismatch (first 496 bytes)\n");
        return TEST_FAIL;
    }
    for (int i = 496; i < BLOCK_SIZE; i++) {
        if (actual[i] != 0) {
            kprintf("[TEST] journal_log_commit: FAIL — trailing byte %d not zero\n", i);
            return TEST_FAIL;
        }
    }

    kprintf("[TEST] journal_log_commit: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 9: Journal multi-txn — three sequential transactions
 * ============================================================ */
static int test_journal_multi_txn(void) {
    uint8_t ref[BLOCK_SIZE];
    uint8_t actual[BLOCK_SIZE];

    block_sync_dev(&test_bdev);
    err_t e = journal_init(&test_bdev, 0);
    if (e != ERR_OK) return TEST_FAIL;

    for (int txn = 0; txn < 3; txn++) {
        kmemset(ref, txn + 1, BLOCK_SIZE);
        uint32_t seq;
        e = journal_start_txn(&test_bdev, 0, &seq);
        if (e != ERR_OK) return TEST_FAIL;
        e = journal_log(&test_bdev, 0, seq, 100 + txn, ref);
        if (e != ERR_OK) return TEST_FAIL;
        e = journal_commit(&test_bdev, 0, seq);
        if (e != ERR_OK) return TEST_FAIL;
    }

    /* Scrub targets and recover */
    for (int txn = 0; txn < 3; txn++) {
        kmemset(actual, 0, BLOCK_SIZE);
        test_bdev.write(&test_bdev, 100 + txn, 1, actual);
    }

    int was_dirty = -1;
    e = journal_recover(&test_bdev, 0, &was_dirty);
    if (e != ERR_OK || was_dirty != 1) return TEST_FAIL;

    /* Verify each block was replayed correctly */
    for (int txn = 0; txn < 3; txn++) {
        kmemset(actual, 0, BLOCK_SIZE);
        test_bdev.read(&test_bdev, 100 + txn, 1, actual);
        uint8_t expected = (uint8_t)(txn + 1);
        for (int i = 0; i < 496; i++) {
            if (actual[i] != expected) return TEST_FAIL;
        }
        for (int i = 496; i < BLOCK_SIZE; i++) {
            if (actual[i] != 0) return TEST_FAIL;
        }
    }

    kprintf("[TEST] journal_multi_txn: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 10: Journal full + checkpoint
 * ============================================================ */
static int test_journal_full(void) {
    uint8_t ref[BLOCK_SIZE];
    uint8_t actual[BLOCK_SIZE];

    block_sync_dev(&test_bdev);
    err_t e = journal_init(&test_bdev, 0);
    if (e != ERR_OK) return TEST_FAIL;

    /* Fill journal with as many transactions as possible.
     * Each txn uses 2 entry slots (DATA + COMMIT). JENT_COUNT=255,
     * so we can fit 127 full transactions before COMMIT has no space. */
    int txn_count = 0;
    while (txn_count < 130) {
        uint32_t seq;
        e = journal_start_txn(&test_bdev, 0, &seq);
        if (e != ERR_OK) break;
        kmemset(ref, (uint8_t)(txn_count & 0xFF), BLOCK_SIZE);
        e = journal_log(&test_bdev, 0, seq, 10 + txn_count, ref);
        if (e != ERR_OK) break;
        e = journal_commit(&test_bdev, 0, seq);
        if (e != ERR_OK) break;
        txn_count++;
    }

    if (txn_count < 1) {
        kprintf("[TEST] journal_full: FAIL — no transactions committed\n");
        return TEST_FAIL;
    }

    /* Checkpoint to free journal space */
    e = journal_checkpoint(&test_bdev, 0);
    if (e != ERR_OK) {
        kprintf("[TEST] journal_full: FAIL — checkpoint returned %d\n", e);
        return TEST_FAIL;
    }

    /* After checkpoint, we should be able to do more transactions */
    uint32_t seq;
    kmemset(ref, 0xAA, BLOCK_SIZE);
    e = journal_start_txn(&test_bdev, 0, &seq);
    if (e != ERR_OK) return TEST_FAIL;
    e = journal_log(&test_bdev, 0, seq, 200, ref);
    if (e != ERR_OK) return TEST_FAIL;
    e = journal_commit(&test_bdev, 0, seq);
    if (e != ERR_OK) return TEST_FAIL;

    /* Scrub and recover */
    kmemset(actual, 0, BLOCK_SIZE);
    test_bdev.write(&test_bdev, 200, 1, actual);

    int was_dirty = -1;
    e = journal_recover(&test_bdev, 0, &was_dirty);
    if (e != ERR_OK || was_dirty != 1) return TEST_FAIL;

    test_bdev.read(&test_bdev, 200, 1, actual);
    for (int i = 0; i < 496; i++) {
        if (actual[i] != 0xAA) return TEST_FAIL;
    }

    kprintf("[TEST] journal_full: PASS (%d txns before checkpoint)\n", txn_count);
    return TEST_PASS;
}

/* ============================================================
 * Test 8: Snapshot take + info
 *
 * Takes a snapshot of the test block device, verifies the
 * info matches expected block count, then frees it.
 * ============================================================ */
static int test_snapshot_take_info(void) {
    /* Write identifiable data so blocks have known content */
    uint8_t wbuf[BLOCK_SIZE];
    kmemset(wbuf, 0x42, BLOCK_SIZE);
    block_sync_dev(&test_bdev);  /* ensure cache is clean before snapshot */
    /* Write one block to give the snapshot something to save */
    block_write(&test_bdev, 0, 1, wbuf);

    int count_before = snapshot_count();
    err_t e = snapshot_take(&test_bdev, "test-snap");
    if (e != ERR_OK) {
        kprintf("[TEST] snapshot_take_info: FAIL — take returned %d\n", e);
        return TEST_FAIL;
    }

    int count_after = snapshot_count();
    if (count_after != count_before + 1) {
        kprintf("[TEST] snapshot_take_info: FAIL — expected %d snapshots, got %d\n",
                count_before + 1, count_after);
        return TEST_FAIL;
    }

    /* Find which slot was used by querying info */
    int found = -1;
    for (int i = 0; i < SNAP_MAX; i++) {
        snap_info_t info;
        kmemset(&info, 0, sizeof(info));
        if (snapshot_info(i, &info) == 0 && info.used) {
            if (kstrcmp(info.desc, "test-snap") == 0) {
                found = i;
                if (info.blocks != TEST_NDISK_BLOCKS) {
                    kprintf("[TEST] snapshot_take_info: FAIL — expected %u blocks, got %u\n",
                            TEST_NDISK_BLOCKS, info.blocks);
                    return TEST_FAIL;
                }
                break;
            }
        }
    }
    if (found < 0) {
        kprintf("[TEST] snapshot_take_info: FAIL — snapshot not found in table\n");
        return TEST_FAIL;
    }

    /* Clean up */
    snapshot_free(found);
    if (snapshot_count() != count_before) {
        kprintf("[TEST] snapshot_take_info: FAIL — count after free mismatch\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] snapshot_take_info: PASS\n");
    return TEST_PASS;
}

void storage_self_test(void) {
    kprintf("[TEST] === Storage self-tests ===\n");

    test_bdev_init();
    /* Register the test device so block cache works */
    block_register(&test_bdev);

    int pass = 0, fail = 0;
    if (test_block_cache_basic() == TEST_PASS) pass++; else fail++;
    if (test_block_cache_writeback() == TEST_PASS) pass++; else fail++;
    if (test_block_register_reject() == TEST_PASS) pass++; else fail++;
    if (test_swap_alloc_free() == TEST_PASS) pass++; else fail++;
    if (test_swap_out_in() == TEST_PASS) pass++; else fail++;
    if (test_journal_init() == TEST_PASS) pass++; else fail++;
    if (test_journal_log_commit() == TEST_PASS) pass++; else fail++;
    if (test_journal_multi_txn() == TEST_PASS) pass++; else fail++;
    if (test_journal_full() == TEST_PASS) pass++; else fail++;
    if (test_snapshot_take_info() == TEST_PASS) pass++; else fail++;

    kprintf("[TEST] === Results: %d pass, %d fail ===\n", pass, fail);
}

#endif /* STORAGE_SELF_TEST */
