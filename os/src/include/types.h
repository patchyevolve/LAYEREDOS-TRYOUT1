#ifndef TYPES_H
#define TYPES_H

#ifndef _STDINT_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
#ifndef _SYS_TYPES_H
typedef uint64_t           size_t;
typedef int64_t            ssize_t;
typedef uint64_t           uintptr_t;
typedef int64_t            intptr_t;
typedef uint32_t           uid_t;
typedef uint32_t           gid_t;
#endif
#endif
#define NULL  ((void*)0)

#define offsetof(T, M) __builtin_offsetof(T, M)
#define container_of(P, T, M) ((T*)((uintptr_t)(P) - offsetof(T, M)))

typedef enum { ERR_OK = 0,
    ERR_GENERAL   = -1,
    ERR_NOMEM     = -2,
    ERR_INVAL     = -3,
    ERR_BADADDR   = -4,
    ERR_BUSY      = -5,
    ERR_TIMEOUT   = -6,
    ERR_AGAIN     = -7,
    ERR_FAULT     = -8,
    ERR_NOSYS     = -9,
    ERR_PERM      = -10,
    ERR_EXIST     = -11,
    ERR_NOENT     = -12,
    ERR_IO        = -13,
    ERR_NOSPACE   = -14,
    ERR_NAMETOOLONG = -15,
    ERR_LOOP      = -16,
    ERR_STALE     = -17,
    ERR_DEADLOCK  = -18,
    ERR_CAP       = -19,
    ERR_BADFD     = -20,
    ERR_NOTCONN   = -21,
    ERR_CONNREFUSED = -22,
} err_t;

#define KERNEL_PHYS_BASE   0x100000
#define KERNEL_VMA_BASE    0xFFFFFFFFC0000000ULL
#define KERNEL_VMA         (KERNEL_VMA_BASE + KERNEL_PHYS_BASE)
#define PHYS_TO_VIRT(P)    ((uintptr_t)(P) + KERNEL_VMA_BASE)
#define VIRT_TO_PHYS(V)    ((uintptr_t)(V) - KERNEL_VMA_BASE)

#define PAGE_SIZE          4096
#define PAGE_SHIFT         12
#define PAGE_MASK          0xFFFFFFFFFFFFF000ULL
#define PAGE_ALIGN(V)      (((V) + PAGE_SIZE - 1) & PAGE_MASK)
#define IS_PAGE_ALIGNED(V) (((V) & (PAGE_SIZE - 1)) == 0)

#define MAX_PRIORITY       255
#define DEFAULT_PRIORITY   128
#define IDLE_PRIORITY      0
#define TIME_SLICE_MS      10

typedef uint64_t cpu_flags_t;

typedef struct spinlock {
    volatile uint64_t lock;
    const char*       name;
    uint64_t          holder;
} spinlock_t;

#ifndef _SYS_TYPES_H
typedef uint32_t uid_t;
typedef uint32_t gid_t;
#endif

// Embedded intrusive doubly-linked list
typedef struct list_head {
    struct list_head* next;
    struct list_head* prev;
} list_head_t;

static inline void list_init(list_head_t* head) {
    head->next = head->prev = head;
}

static inline void list_add_tail(list_head_t* head, list_head_t* node) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static inline void list_del(list_head_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = node;
}

#endif
