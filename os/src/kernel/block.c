#include "kernel.h"
#include "block.h"

#define MAX_BLOCK_DEVICES 8
#define BLOCK_CACHE_SIZE  64

static block_dev_t block_devs[MAX_BLOCK_DEVICES];
static int block_dev_count = 0;

/* Write-back sector cache with LRU eviction */
typedef struct {
    block_dev_t* dev;
    uint64_t     lba;
    uint8_t      data[BLOCK_SIZE];
    int          valid;
    int          dirty;
    uint64_t     access_time;
} cache_line_t;

static cache_line_t block_cache[BLOCK_CACHE_SIZE];
static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;
static uint64_t cache_writes = 0;
static uint64_t cache_clock = 0;

/* Find matching cache line (updates LRU time) */
static cache_line_t* cache_lookup(block_dev_t* dev, uint64_t lba) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        cache_line_t* cl = &block_cache[i];
        if (cl->valid && cl->dev == dev && cl->lba == lba) {
            cl->access_time = ++cache_clock;
            return cl;
        }
    }
    return NULL;
}

/* Evict the LRU entry (prefer non-dirty), write-back if dirty */
static cache_line_t* cache_evict(block_dev_t* dev, uint64_t lba) {
    int best_valid = -1;
    uint64_t oldest_valid = (uint64_t)-1;
    int best_clean = -1;
    uint64_t oldest_clean = (uint64_t)-1;

    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        cache_line_t* cl = &block_cache[i];
        if (cl->valid && cl->access_time < oldest_valid) {
            oldest_valid = cl->access_time;
            best_valid = i;
        }
        if (cl->valid && !cl->dirty && cl->access_time < oldest_clean) {
            oldest_clean = cl->access_time;
            best_clean = i;
        }
    }

    /* Prefer a clean entry; if all dirty, evict oldest */
    int idx = (best_clean >= 0) ? best_clean : best_valid;
    if (idx < 0) {
        /* No valid entries — use slot 0 */
        idx = 0;
    }

    cache_line_t* cl = &block_cache[idx];
    if (cl->valid && cl->dirty && cl->dev && cl->dev->write) {
        cl->dev->write(cl->dev, cl->lba, 1, cl->data);
        cache_writes++;
    }

    cl->valid = 1;
    cl->dev = dev;
    cl->lba = lba;
    cl->dirty = 0;
    cl->access_time = ++cache_clock;
    return cl;
}

err_t block_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    if (!dev || !dev->read) return ERR_INVAL;
    uint8_t* p = (uint8_t*)buf;
    for (uint8_t i = 0; i < count; i++) {
        cache_line_t* cl = cache_lookup(dev, lba + i);
        if (cl) {
            kmemcpy(p + (uint64_t)i * BLOCK_SIZE, cl->data, BLOCK_SIZE);
            cache_hits++;
        } else {
            cl = cache_evict(dev, lba + i);
            err_t e = dev->read(dev, lba + i, 1, cl->data);
            if (e) return e;
            kmemcpy(p + (uint64_t)i * BLOCK_SIZE, cl->data, BLOCK_SIZE);
            cache_misses++;
        }
    }
    return ERR_OK;
}

err_t block_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    if (!dev || !dev->write) return ERR_INVAL;
    const uint8_t* p = (const uint8_t*)buf;
    for (uint8_t i = 0; i < count; i++) {
        cache_line_t* cl = cache_lookup(dev, lba + i);
        if (!cl) {
            cl = cache_evict(dev, lba + i);
        }
        kmemcpy(cl->data, p + (uint64_t)i * BLOCK_SIZE, BLOCK_SIZE);
        cl->dirty = 1;
    }
    return ERR_OK;
}

err_t block_sync(void) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        cache_line_t* cl = &block_cache[i];
        if (cl->valid && cl->dirty && cl->dev && cl->dev->write) {
            err_t e = cl->dev->write(cl->dev, cl->lba, 1, cl->data);
            if (e) return e;
            cl->dirty = 0;
            cache_writes++;
        }
    }
    return ERR_OK;
}

err_t block_sync_dev(block_dev_t* dev) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        cache_line_t* cl = &block_cache[i];
        if (cl->valid && cl->dirty && cl->dev == dev && cl->dev->write) {
            err_t e = cl->dev->write(cl->dev, cl->lba, 1, cl->data);
            if (e) return e;
            cl->dirty = 0;
            cache_writes++;
        }
    }
    return ERR_OK;
}

void block_cache_stats(void) {
    int valid = 0, dirty = 0;
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        if (block_cache[i].valid) valid++;
        if (block_cache[i].dirty) dirty++;
    }
    kprintf("[BLOCK] cache: %llu hits, %llu misses, %llu writes-back, "
            "%d/%d valid, %d dirty\n",
            cache_hits, cache_misses, cache_writes,
            valid, BLOCK_CACHE_SIZE, dirty);
}

int block_register(block_dev_t* dev) {
    if (!dev || block_dev_count >= MAX_BLOCK_DEVICES) return -1;
    block_devs[block_dev_count++] = *dev;
    return block_dev_count - 1;
}

int block_count(void) { return block_dev_count; }

block_dev_t* block_get(int index) {
    if (index < 0 || index >= block_dev_count) return NULL;
    return &block_devs[index];
}

block_dev_t* block_find(const char* name) {
    for (int i = 0; i < block_dev_count; i++) {
        if (kstrcmp(block_devs[i].name, name) == 0)
            return &block_devs[i];
    }
    return NULL;
}
