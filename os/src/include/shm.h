#ifndef SHM_H
#define SHM_H

#include "types.h"
#include "vfs.h"
#include "sync.h"

#define SHM_NAME_MAX  64
#define SHM_MAX_OBJS  32

typedef struct shm_object {
    char      name[SHM_NAME_MAX];
    int       used;
    uint64_t  size;
    uintptr_t* pages;
    int       refcount;
    int       deleted;
    spinlock_t lock;
} shm_object_t;

void   shm_init(void);
int    sys_shm_open(const char* name, int oflag, int mode);
int    sys_shm_unlink(const char* name);

#endif
