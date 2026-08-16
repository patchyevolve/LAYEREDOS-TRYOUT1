#include "kernel.h"
#include "shm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vfs.h"
#include "fcntl.h"
#include "errno.h"
#include "process.h"
#include "sched.h"

static shm_object_t shm_objs[SHM_MAX_OBJS];
static spinlock_t   shm_global_lock;

void shm_init(void) {
    spinlock_init(&shm_global_lock, "shm-global");
    kprintf("[SHM] shared memory initialized (%d objects max)\n", SHM_MAX_OBJS);
}

static shm_object_t* shm_find(const char* name) {
    for (int i = 0; i < SHM_MAX_OBJS; i++) {
        if (shm_objs[i].used && kstrcmp(shm_objs[i].name, name) == 0)
            return &shm_objs[i];
    }
    return NULL;
}

static int shm_alloc_slot(const char* name) {
    for (int i = 0; i < SHM_MAX_OBJS; i++) {
        if (!shm_objs[i].used) {
            shm_objs[i].used = 1;
            kstrncpy(shm_objs[i].name, name, SHM_NAME_MAX - 1);
            shm_objs[i].name[SHM_NAME_MAX - 1] = 0;
            shm_objs[i].size = 0;
            shm_objs[i].pages = NULL;
            shm_objs[i].refcount = 0;
            shm_objs[i].deleted = 0;
            spinlock_init(&shm_objs[i].lock, "shm-obj");
            return i;
        }
    }
    return -1;
}

static int shm_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int shm_vfs_close(vfs_node_t* node) {
    if (!node || !node->private_data) return -1;
    shm_object_t* obj = (shm_object_t*)node->private_data;
    cpu_flags_t _sf;
    spinlock_acquire(&obj->lock, &_sf);
    obj->refcount--;
    int should_free = (obj->refcount <= 0 && obj->deleted);
    spinlock_release(&obj->lock, _sf);
    if (should_free) {
        if (obj->pages) {
            uint64_t np = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint64_t i = 0; i < np; i++) {
                if (obj->pages[i]) pmm_free_page(obj->pages[i]);
            }
            kfree(obj->pages);
            obj->pages = NULL;
        }
        obj->used = 0;
        obj->size = 0;
    }
    return 0;
}

static int64_t shm_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    if (!node || !node->private_data || !buf) return -1;
    shm_object_t* obj = (shm_object_t*)node->private_data;
    cpu_flags_t _sf;
    spinlock_acquire(&obj->lock, &_sf);
    if (offset >= obj->size) { spinlock_release(&obj->lock, _sf); return 0; }
    uint64_t avail = obj->size - offset;
    if (count > avail) count = avail;
    uint64_t done = 0;
    while (done < count) {
        uint64_t pg = (offset + done) / PAGE_SIZE;
        uint64_t pg_off = (offset + done) % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - pg_off;
        if (chunk > count - done) chunk = count - done;
        if (!obj->pages || !obj->pages[pg]) { done += chunk; continue; }
        kmemcpy((uint8_t*)buf + done, (void*)PHYS_TO_VIRT(obj->pages[pg]) + pg_off, chunk);
        done += chunk;
    }
    spinlock_release(&obj->lock, _sf);
    return (int64_t)count;
}

static int64_t shm_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    if (!node || !node->private_data || !buf) return -1;
    shm_object_t* obj = (shm_object_t*)node->private_data;
    cpu_flags_t _sf;
    spinlock_acquire(&obj->lock, &_sf);
    if (offset >= obj->size) { spinlock_release(&obj->lock, _sf); return 0; }
    uint64_t avail = obj->size - offset;
    if (count > avail) count = avail;
    uint64_t done = 0;
    while (done < count) {
        uint64_t pg = (offset + done) / PAGE_SIZE;
        uint64_t pg_off = (offset + done) % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - pg_off;
        if (chunk > count - done) chunk = count - done;
        if (!obj->pages || !obj->pages[pg]) { done += chunk; continue; }
        kmemcpy((void*)PHYS_TO_VIRT(obj->pages[pg]) + pg_off, (const uint8_t*)buf + done, chunk);
        done += chunk;
    }
    spinlock_release(&obj->lock, _sf);
    return (int64_t)count;
}

static int shm_vfs_truncate(vfs_node_t* node, uint64_t size) {
    if (!node || !node->private_data) return -1;
    shm_object_t* obj = (shm_object_t*)node->private_data;

    uint64_t old_np = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t new_np = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (new_np < old_np) {
        cpu_flags_t _sf;
        spinlock_acquire(&obj->lock, &_sf);
        for (uint64_t i = new_np; i < old_np; i++) {
            if (obj->pages[i]) { pmm_free_page(obj->pages[i]); obj->pages[i] = 0; }
        }
        obj->size = size;
        spinlock_release(&obj->lock, _sf);
        return 0;
    }

    if (new_np > old_np) {
        cpu_flags_t _sf;
        spinlock_acquire(&obj->lock, &_sf);
        uintptr_t* new_pages = (uintptr_t*)kmalloc(new_np * sizeof(uintptr_t));
        if (!new_pages) { spinlock_release(&obj->lock, _sf); return -1; }
        kmemset(new_pages, 0, new_np * sizeof(uintptr_t));
        if (obj->pages) {
            kmemcpy(new_pages, obj->pages, old_np * sizeof(uintptr_t));
            kfree(obj->pages);
        }
        for (uint64_t i = old_np; i < new_np; i++) {
            uint64_t p = pmm_alloc_page();
            if (!p) {
                for (uint64_t j = old_np; j < i; j++)
                    if (new_pages[j]) pmm_free_page(new_pages[j]);
                kfree(new_pages);
                spinlock_release(&obj->lock, _sf);
                return -1;
            }
            kmemset((void*)PHYS_TO_VIRT(p), 0, PAGE_SIZE);
            new_pages[i] = p;
        }
        obj->pages = new_pages;
        obj->size = size;
        spinlock_release(&obj->lock, _sf);
    }
    return 0;
}

static uint64_t shm_vfs_get_page(vfs_node_t* node, uint64_t offset) {
    if (!node || !node->private_data) return 0;
    shm_object_t* obj = (shm_object_t*)node->private_data;
    uint64_t pg = offset / PAGE_SIZE;
    if (pg * PAGE_SIZE >= obj->size) return 0;
    if (!obj->pages || !obj->pages[pg]) return 0;
    return (uint64_t)obj->pages[pg];
}

static int shm_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !node->private_data) return -1;
    shm_object_t* obj = (shm_object_t*)node->private_data;
    cpu_flags_t _sf;
    spinlock_acquire(&obj->lock, &_sf);
    kmemset(st, 0, sizeof(*st));
    st->size = obj->size;
    st->mode = 0644;
    st->uid  = current_thread && current_thread->proc ? current_thread->proc->euid : 0;
    st->gid  = current_thread && current_thread->proc ? current_thread->proc->egid : 0;
    spinlock_release(&obj->lock, _sf);
    return 0;
}

static vfs_file_ops_t shm_ops = {
    .open     = shm_vfs_open,
    .close    = shm_vfs_close,
    .read     = shm_vfs_read,
    .write    = shm_vfs_write,
    .truncate = shm_vfs_truncate,
    .stat     = shm_vfs_stat,
    .get_page = shm_vfs_get_page,
};

static vfs_fs_t shm_fs = {
    .name = "shm",
    .ops  = &shm_ops,
};

int sys_shm_open(const char* name, int oflag, int mode) {
    (void)mode;
    char kname[SHM_NAME_MAX];
    if (!name) return ERR_INVAL;

    const char* src = name;
    while (*src == '/') src++;
    size_t len = kstrlen(src);
    if (len == 0 || len >= SHM_NAME_MAX) return ERR_INVAL;
    kmemset(kname, 0, SHM_NAME_MAX);
    kmemcpy(kname, src, len);

    cpu_flags_t _sf;
    spinlock_acquire(&shm_global_lock, &_sf);

    shm_object_t* obj = shm_find(kname);
    if (obj && (oflag & O_EXCL)) {
        spinlock_release(&shm_global_lock, _sf);
        return ERR_EXIST;
    }

    if (!obj) {
        if (!(oflag & O_CREAT)) {
            spinlock_release(&shm_global_lock, _sf);
            return ERR_NOENT;
        }
        int slot = shm_alloc_slot(kname);
        if (slot < 0) {
            spinlock_release(&shm_global_lock, _sf);
            return ERR_NOMEM;
        }
        obj = &shm_objs[slot];
    }

    int rw_flag = O_RDWR | (oflag & O_NONBLOCK);
    uint64_t flags = rw_flag;
    if (oflag & O_TRUNC) {
        uint64_t old_np = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < old_np; i++) {
            if (obj->pages[i]) { pmm_free_page(obj->pages[i]); obj->pages[i] = 0; }
        }
        obj->size = 0;
        if (obj->pages) { kfree(obj->pages); obj->pages = NULL; }
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        if (!obj->deleted && obj->refcount <= 0) { obj->used = 0; }
        spinlock_release(&shm_global_lock, _sf);
        return ERR_NOMEM;
    }
    kmemset(node, 0, sizeof(*node));
    kstrncpy(node->name, kname, VFS_MAX_NAME - 1);
    node->name[VFS_MAX_NAME - 1] = 0;
    node->fs = &shm_fs;
    node->private_data = (void*)obj;
    node->dynamic = 1;
    node->refcount = 1;
    node->destructor = NULL;

    int fd = -1;
    vfs_fd_t* ft = vfs_get_fd_table();
    cpu_flags_t _vsf;
    spinlock_acquire(&vfs_global_lock, &_vsf);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node = node;
            ft[i].offset = 0;
            ft[i].flags = flags;
            ft[i].used = 1;
            __sync_fetch_and_add(&node->refcount, 1);
            fd = i;
            break;
        }
    }
    spinlock_release(&vfs_global_lock, _vsf);

    if (fd < 0) {
        kfree(node);
        if (!obj->deleted && obj->refcount <= 0) { obj->used = 0; }
        spinlock_release(&shm_global_lock, _sf);
        return ERR_NOMEM;
    }

    obj->refcount++;
    spinlock_release(&shm_global_lock, _sf);
    return fd;
}

int sys_shm_unlink(const char* name) {
    char kname[SHM_NAME_MAX];
    if (!name) return ERR_INVAL;
    const char* src = name;
    while (*src == '/') src++;
    size_t len = kstrlen(src);
    if (len == 0 || len >= SHM_NAME_MAX) return ERR_INVAL;
    kmemset(kname, 0, SHM_NAME_MAX);
    kmemcpy(kname, src, len);

    cpu_flags_t _sf;
    spinlock_acquire(&shm_global_lock, &_sf);

    shm_object_t* obj = shm_find(kname);
    if (!obj) {
        spinlock_release(&shm_global_lock, _sf);
        return ERR_NOENT;
    }

    obj->deleted = 1;
    int should_free = (obj->refcount <= 0);

    if (should_free) {
        if (obj->pages) {
            uint64_t np = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint64_t i = 0; i < np; i++) {
                if (obj->pages[i]) pmm_free_page(obj->pages[i]);
            }
            kfree(obj->pages);
            obj->pages = NULL;
        }
        obj->used = 0;
        obj->size = 0;
    }

    spinlock_release(&shm_global_lock, _sf);
    return 0;
}
