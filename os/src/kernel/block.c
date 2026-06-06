#include "kernel.h"
#include "block.h"

#define MAX_BLOCK_DEVICES 8
#define BLOCK_CACHE_SIZE  64

static block_dev_t block_devs[MAX_BLOCK_DEVICES];
static int block_dev_count = 0;

/* Simple sector cache */
typedef struct {
    uint64_t    dev_hash;
    uint64_t    lba;
    uint8_t     data[BLOCK_SIZE];
    int         valid;
    int         dirty;
} cache_line_t;

static cache_line_t block_cache[BLOCK_CACHE_SIZE];
static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;

static uint64_t ptr_hash(void* p) {
    return (uint64_t)(uintptr_t)p;
}

static cache_line_t* cache_lookup(block_dev_t* dev, uint64_t lba) {
    uint64_t h = ptr_hash(dev);
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        uint64_t idx = (h ^ lba ^ (uint64_t)i) % BLOCK_CACHE_SIZE;
        cache_line_t* cl = &block_cache[idx];
        if (cl->valid && cl->dev_hash == h && cl->lba == lba)
            return cl;
    }
    return NULL;
}

static cache_line_t* cache_evict(block_dev_t* dev, uint64_t lba) {
    uint64_t h = ptr_hash(dev);
    uint64_t idx = (h ^ lba) % BLOCK_CACHE_SIZE;
    cache_line_t* cl = &block_cache[idx];
    if (cl->valid && cl->dirty) {
        block_dev_t* old_dev = NULL;
        for (int i = 0; i < block_dev_count; i++) {
            if (ptr_hash(&block_devs[i]) == cl->dev_hash) {
                old_dev = &block_devs[i];
                break;
            }
        }
        if (old_dev && old_dev->write)
            old_dev->write(old_dev, cl->lba, 1, cl->data);
    }
    cl->valid = 1;
    cl->dev_hash = h;
    cl->lba = lba;
    cl->dirty = 0;
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
            err_t e = dev->read(dev, lba + i, 1, p + (uint64_t)i * BLOCK_SIZE);
            if (e) return e;
            cl = cache_evict(dev, lba + i);
            kmemcpy(cl->data, p + (uint64_t)i * BLOCK_SIZE, BLOCK_SIZE);
            cache_misses++;
        }
    }
    return ERR_OK;
}

err_t block_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    if (!dev || !dev->write) return ERR_INVAL;
    const uint8_t* p = (const uint8_t*)buf;
    for (uint8_t i = 0; i < count; i++) {
        cache_line_t* cl = cache_evict(dev, lba + i);
        kmemcpy(cl->data, p + (uint64_t)i * BLOCK_SIZE, BLOCK_SIZE);
        err_t e = dev->write(dev, lba + i, 1, cl->data);
        if (e) return e;
    }
    return ERR_OK;
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
