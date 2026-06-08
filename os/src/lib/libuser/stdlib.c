#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/* atexit support */
#define ATEXIT_MAX 32

static struct {
    void (*func)(void);
} atexit_handlers[ATEXIT_MAX];
static int atexit_count = 0;
static int exiting = 0;

int atexit(void (*func)(void)) {
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_handlers[atexit_count++].func = func;
    return 0;
}

static void call_atexit_handlers(void) {
    if (exiting) return;
    exiting = 1;
    while (atexit_count > 0)
        atexit_handlers[--atexit_count].func();
}

void exit(int status) {
    call_atexit_handlers();
    _exit(status);
}

void __libc_init(void) {
    atexit_count = 0;
    exiting = 0;
}

long atol(const char* s) {
    long v = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        int d = *s - '0';
        if (v > LONG_MAX / 10 || (v == LONG_MAX / 10 && d > LONG_MAX % 10))
            return sign > 0 ? LONG_MAX : LONG_MIN;
        v = v * 10 + d;
        s++;
    }
    return sign * v;
}

int atoi(const char* s) {
    return (int)atol(s);
}

int abs(int n) {
    return n < 0 ? -n : n;
}

/* Simple free-list based malloc */
typedef struct block {
    size_t size;
    struct block* next;
    int free;
} block_t;

static block_t* heap_start = NULL;
static block_t* last_alloc = NULL;

static block_t* find_free_block(block_t** last, size_t size) {
    block_t* cur = heap_start;
    *last = NULL;
    while (cur) {
        if (cur->free && cur->size >= size) return cur;
        *last = cur;
        cur = cur->next;
    }
    return NULL;
}

static block_t* extend_heap(block_t* last, size_t size) {
    size_t total = sizeof(block_t) + size;
    if (total > LONG_MAX) return NULL;
    block_t* block = (block_t*)sbrk((long)total);
    if ((long)block < 0) return NULL;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    if (last) last->next = block;
    return block;
}

#define MIN_SPLIT 32

static void split_block(block_t* block, size_t size) {
    if (block->size < size + sizeof(block_t) + MIN_SPLIT) return;
    block_t* remainder = (block_t*)((char*)(block + 1) + size);
    remainder->size = block->size - size - sizeof(block_t);
    remainder->next = block->next;
    remainder->free = 1;
    block->size = size;
    block->next = remainder;
}

void* malloc(size_t size) {
    if (size == 0) size = 1;
    block_t* block;

    if (!heap_start) {
        block = extend_heap(NULL, size);
        if (!block) { errno = ENOMEM; return NULL; }
        heap_start = block;
        last_alloc = block;
    } else {
        block_t* last = heap_start;
        block = find_free_block(&last, size);
        if (block) {
            block->free = 0;
            split_block(block, size);
        } else {
            block = extend_heap(last, size);
            if (!block) { errno = ENOMEM; return NULL; }
        }
    }
    return (void*)(block + 1);
}

void free(void* ptr) {
    if (!ptr) return;
    block_t* block = (block_t*)ptr - 1;
    block->free = 1;

    while (block->next && block->next->free) {
        block_t* next = block->next;
        block->size += sizeof(block_t) + next->size;
        block->next = next->next;
    }

    if (heap_start && block != heap_start) {
        block_t* prev = heap_start;
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->free) {
            prev->size += sizeof(block_t) + block->size;
            prev->next = block->next;
        }
    }
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return malloc(0);
    if (nmemb > (size_t)-1 / size) { errno = ENOMEM; return NULL; }
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) {
        unsigned char* cp = (unsigned char*)p;
        for (size_t i = 0; i < total; i++) cp[i] = 0;
    }
    return p;
}

void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }
    block_t* block = (block_t*)ptr - 1;
    size_t old_size = block->size;
    if (new_size <= old_size) return ptr;
    void* new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    unsigned char* src = (unsigned char*)ptr;
    unsigned char* dst = (unsigned char*)new_ptr;
    for (size_t i = 0; i < old_size; i++) dst[i] = src[i];
    free(ptr);
    return new_ptr;
}
