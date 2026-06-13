#include "kernel.h"
#include "journal.h"
#include "block.h"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t head;
    uint32_t tail;
    uint32_t flags;
    uint8_t  pad[488];
    uint32_t checksum;
} jsb_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t seq;
    uint32_t block;
    uint8_t  data[496];
    uint32_t checksum;
} jent_data_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t seq;
    uint8_t  pad[500];
    uint32_t checksum;
} jent_commit_t;

#define JENT_DATA 2
#define JENT_COMMIT 3

static uint32_t journal_checksum(const uint32_t* p, int count) {
    uint32_t c = JOURNAL_CHECKSUM_SEED;
    for (int i = 0; i < count; i++) c ^= p[i];
    return c;
}

#define JENT_DATA_WORDS  (sizeof(jent_data_t) / 4 - 1)
#define JENT_COMMIT_WORDS (sizeof(jent_commit_t) / 4 - 1)
#define JSB_WORDS        (sizeof(jsb_t) / 4 - 1)

static int jent_next(int i) {
    return (i + 1) % JENT_COUNT;
}

static int jspace(jsb_t* jsb) {
    if (jsb->head == jsb->tail) return JENT_COUNT - 1;
    if (jsb->tail > jsb->head)
        return JENT_COUNT - 1 - (int)(jsb->tail - jsb->head);
    return (int)(jsb->head - jsb->tail) - 1;
}

err_t journal_init(block_dev_t* bdev, uint32_t start) {
    jsb_t jsb;
    kmemset(&jsb, 0, sizeof(jsb));
    jsb.magic = JOURNAL_MAGIC;
    jsb.seq = 1;
    jsb.head = 0;
    jsb.tail = 0;
    jsb.flags = 0;
    jsb.checksum = journal_checksum((const uint32_t*)&jsb, JSB_WORDS);
    err_t e = bdev->write(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;
    kprintf("[JOURNAL] Initialised at block %u (%u slots)\n", start, JENT_COUNT);
    return ERR_OK;
}

err_t journal_recover(block_dev_t* bdev, uint32_t start, int* was_dirty) {
    jsb_t jsb;
    err_t e = bdev->read(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;
    if (jsb.magic != JOURNAL_MAGIC) {
        kprintf("[JOURNAL] No valid JSB, skipping recovery\n");
        if (was_dirty) *was_dirty = 0;
        return ERR_OK;
    }

    /* Verify JSB checksum */
    {
        uint32_t stored_cs = jsb.checksum;
        jsb.checksum = 0;
        if (journal_checksum((const uint32_t*)&jsb, JSB_WORDS) != stored_cs) {
            kprintf("[JOURNAL] JSB checksum mismatch, journal may be corrupt\n");
            jsb.head = 0;
            jsb.tail = 0;
            jsb.checksum = stored_cs;
        }
        jsb.checksum = stored_cs;
    }

    int recovered = 0;
    int i = (int)jsb.head;
    while (i != (int)jsb.tail) {
        uint8_t buf[BLOCK_SIZE];
        e = bdev->read(bdev, JENT_BLOCK(start, i), 1, buf);
        if (e) break;

        jent_data_t* de = (jent_data_t*)buf;
        uint32_t type = de->type;
        uint32_t seq = de->seq;

        /* Verify entry checksum */
        uint32_t stored_cs = de->checksum;
        de->checksum = 0;
        uint32_t calc_cs = 0;
        if (type == JENT_DATA) {
            calc_cs = journal_checksum((const uint32_t*)de, JENT_DATA_WORDS);
        } else if (type == JENT_COMMIT) {
            calc_cs = journal_checksum((const uint32_t*)de, JENT_COMMIT_WORDS);
        }
        de->checksum = stored_cs;

        if (calc_cs != stored_cs) {
            kprintf("[JOURNAL] Entry %d checksum mismatch (type=%u seq=%u), skipping\n", i, type, seq);
            i = jent_next(i);
            continue;
        }

        if (type == JENT_COMMIT) {
            int scan = (int)jsb.head;
            while (scan != i) {
                uint8_t sbuf[BLOCK_SIZE];
                bdev->read(bdev, JENT_BLOCK(start, scan), 1, sbuf);
                jent_data_t* sde = (jent_data_t*)sbuf;
                if (sde->type == JENT_DATA && sde->seq == seq) {
                    uint8_t block_buf[BLOCK_SIZE];
                    bdev->read(bdev, sde->block, 1, block_buf);
                    kmemcpy(block_buf, sde->data, sizeof(sde->data));
                    bdev->write(bdev, sde->block, 1, block_buf);
                    recovered++;
                }
                scan = jent_next(scan);
            }
        }
        i = jent_next(i);
    }

    if (recovered > 0) {
        kprintf("[JOURNAL] Recovered %d blocks\n", recovered);
        if (was_dirty) *was_dirty = 1;
    } else {
        kprintf("[JOURNAL] Empty, no recovery needed\n");
        if (was_dirty) *was_dirty = 0;
    }

    jsb.head = jsb.tail;
    jsb.checksum = 0;
    jsb.checksum = journal_checksum((const uint32_t*)&jsb, JSB_WORDS);
    bdev->write(bdev, JSB_BLOCK(start), 1, &jsb);
    return ERR_OK;
}

err_t journal_start_txn(block_dev_t* bdev, uint32_t start, uint32_t* seq) {
    jsb_t jsb;
    err_t e = bdev->read(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;
    *seq = jsb.seq;
    return ERR_OK;
}

err_t journal_log(block_dev_t* bdev, uint32_t start, uint32_t seq, uint32_t block, const void* data) {
    jsb_t jsb;
    err_t e = bdev->read(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;

    if (jspace(&jsb) < 2) return ERR_NOSPACE;

    jent_data_t de;
    de.type = JENT_DATA;
    de.seq = seq;
    de.block = block;
    kmemset(de.data, 0, sizeof(de.data));
    kmemcpy(de.data, data, sizeof(de.data));
    de.checksum = 0;
    de.checksum = journal_checksum((const uint32_t*)&de, JENT_DATA_WORDS);

    int slot = (int)jsb.tail;
    e = bdev->write(bdev, JENT_BLOCK(start, slot), 1, &de);
    if (e) return e;

    /* Barrier: flush journal entry to storage before updating JSB */
    block_flush(bdev, JENT_BLOCK(start, slot), 1);

    jsb.tail = (uint32_t)jent_next((int)jsb.tail);
    jsb.checksum = 0;
    jsb.checksum = journal_checksum((const uint32_t*)&jsb, JSB_WORDS);
    e = bdev->write(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;
    return ERR_OK;
}

err_t journal_commit(block_dev_t* bdev, uint32_t start, uint32_t seq) {
    jsb_t jsb;
    err_t e = bdev->read(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;

    if (jspace(&jsb) < 1) return ERR_NOSPACE;

    jent_commit_t ce;
    kmemset(&ce, 0, sizeof(ce));
    ce.type = JENT_COMMIT;
    ce.seq = seq;
    ce.checksum = journal_checksum((const uint32_t*)&ce, JENT_COMMIT_WORDS);

    int slot = (int)jsb.tail;
    e = bdev->write(bdev, JENT_BLOCK(start, slot), 1, &ce);
    if (e) return e;

    /* Barrier: ensure commit entry is on storage before JSB update */
    block_flush(bdev, JENT_BLOCK(start, slot), 1);

    jsb.tail = (uint32_t)jent_next((int)jsb.tail);
    jsb.seq = seq + 1;
    jsb.checksum = 0;
    jsb.checksum = journal_checksum((const uint32_t*)&jsb, JSB_WORDS);
    e = bdev->write(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;
    return ERR_OK;
}

err_t journal_checkpoint(block_dev_t* bdev, uint32_t start) {
    jsb_t jsb;
    err_t e = bdev->read(bdev, JSB_BLOCK(start), 1, &jsb);
    if (e) return e;

    if (jsb.head == jsb.tail) return ERR_OK;

    uint32_t scan = jsb.head;
    uint32_t last_commit = jsb.head;

    while (scan != jsb.tail) {
        uint8_t buf[BLOCK_SIZE];
        e = bdev->read(bdev, JENT_BLOCK(start, scan), 1, buf);
        if (e) break;
        uint32_t* hdr = (uint32_t*)buf;
        if (hdr[0] == JENT_COMMIT) {
            last_commit = (uint32_t)jent_next((int)scan);
        }
        scan = (uint32_t)jent_next((int)scan);
    }

    if (last_commit != jsb.head) {
        jsb.head = last_commit;
        jsb.checksum = 0;
        jsb.checksum = journal_checksum((const uint32_t*)&jsb, JSB_WORDS);
        bdev->write(bdev, JSB_BLOCK(start), 1, &jsb);
    }
    return ERR_OK;
}
