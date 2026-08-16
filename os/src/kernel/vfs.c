#include "kernel.h"
#include "vfs.h"
#include "security.h"
#include "hal.h"
#include "process.h"
#include "sched.h"
#include "sync.h"
#include "kmalloc.h"
#include "net.h"

/* Kernel fallback fd table (used by kernel threads with no process) */
static vfs_fd_t kernel_fd_table[VFS_MAX_FDS];

/* Dentry cache for accelerated path resolution */
#define DENTRY_CACHE_SIZE 64
typedef struct {
    vfs_node_t* parent;
    uint32_t    name_hash;
    vfs_node_t* child;
    int         age;
} dentry_entry_t;
static dentry_entry_t dentry_cache[DENTRY_CACHE_SIZE];
static int dentry_cache_age = 0;
static int dentry_cache_count = 0;
spinlock_t vfs_global_lock;

vfs_fd_t* vfs_get_fd_table(void) {
    if (current_thread && current_thread->proc)
        return current_thread->proc->fds;
    return kernel_fd_table;
}
static vfs_node_t root_node;
static vfs_fs_t* mounted_fs = NULL;

#define VFS_MAX_MOUNTS 8
static struct {
    char prefix[64];
    vfs_fs_t* fs;
} mount_table[VFS_MAX_MOUNTS];
static int mount_count = 0;

static vfs_fs_t* vfs_fs_for_path(const char* path) {
    if (!path) return mounted_fs;
    vfs_fs_t* best = mounted_fs;
    int best_len = 0;
    for (int i = 0; i < mount_count; i++) {
        int len = kstrlen(mount_table[i].prefix);
        if (len == 0) continue;
        if (kstrncmp(path, mount_table[i].prefix, (size_t)len) == 0) {
            char c = path[len];
            if (c == '/' || c == '\0') {
                if (len > best_len) {
                    best = mount_table[i].fs;
                    best_len = len;
                }
            }
        }
    }
    return best;
}



err_t vfs_init(void) {
    spinlock_init(&vfs_global_lock, "vfs_global_lock");
    kmemset(&root_node, 0, sizeof(root_node));
    kstrncpy(root_node.name, "/", VFS_MAX_NAME - 1);
    root_node.flags = 1;

    for (int i = 0; i < VFS_MAX_FDS; i++)
        kernel_fd_table[i].used = 0;

    kmemset(dentry_cache, 0, sizeof(dentry_cache));
    dentry_cache_age = 0;
    dentry_cache_count = 0;

    kprintf("[VFS] Virtual filesystem initialized\n");
    return ERR_OK;
}

int vfs_register_fs(vfs_fs_t* fs) {
    if (!fs) return -1;
    mounted_fs = fs;
    root_node.fs = fs;
    if (fs->root) {
        fs->root->parent = &root_node;
        root_node.children = fs->root;
    }
    return 0;
}

err_t vfs_mount(const char* path, vfs_fs_t* fs) {
    if (!fs) return ERR_INVAL;
    if (mount_count < VFS_MAX_MOUNTS) {
        kstrncpy(mount_table[mount_count].prefix, path ? path : "/", 63);
        mount_table[mount_count].fs = fs;
        mount_count++;
    }
    if (!mounted_fs || kstrcmp(path, "/") == 0) {
        mounted_fs = fs;
        root_node.fs = fs;
        if (fs->root) {
            fs->root->parent = &root_node;
            root_node.children = fs->root;
        }
    }
    return ERR_OK;
}

/* Symlink recursion limit */
#define VFS_MAX_SYMLINKS 40

static uint32_t dentry_hash(const char* name, int len) {
    uint32_t h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) + (uint8_t)name[i];
    return h;
}

static vfs_node_t* dentry_lookup(vfs_node_t* parent, const char* name, int len) {
    uint32_t h = dentry_hash(name, len);
    for (int i = 0; i < DENTRY_CACHE_SIZE; i++) {
        dentry_entry_t* e = &dentry_cache[i];
        if (e->child && e->parent == parent && e->name_hash == h) {
            if (kstrncmp(e->child->name, name, (size_t)len) == 0 &&
                e->child->name[len] == '\0') {
                e->age = ++dentry_cache_age;
                return e->child;
            }
        }
    }
    return NULL;
}

static void dentry_add(vfs_node_t* parent, vfs_node_t* child) {
    int slot = dentry_cache_count < DENTRY_CACHE_SIZE
               ? dentry_cache_count++
               : 0;
    /* Find LRU slot */
    if (slot == 0 && dentry_cache_count >= DENTRY_CACHE_SIZE) {
        int oldest = 0;
        for (int i = 1; i < DENTRY_CACHE_SIZE; i++) {
            if (dentry_cache[i].age < dentry_cache[oldest].age)
                oldest = i;
        }
        slot = oldest;
    }
    dentry_cache[slot].parent    = parent;
    dentry_cache[slot].name_hash = dentry_hash(child->name, kstrlen(child->name));
    dentry_cache[slot].child     = child;
    dentry_cache[slot].age       = ++dentry_cache_age;
}



static vfs_node_t* vfs_find_flags(const char* path, int follow) {
    if (!path) return NULL;

    /* Buffer for symlink path reconstruction (kept at function scope) */
    char pathbuf[512];

    /* Follow symlinks up to VFS_MAX_SYMLINKS deep */
    int depth = 0;
    while (depth < VFS_MAX_SYMLINKS) {
        int found_symlink = 0;

        /* Determine which filesystem owns this path */
        vfs_fs_t* fs = vfs_fs_for_path(path);
        if (!fs) return NULL;

        const char* inner_path = path;
        for (int i = 0; i < mount_count; i++) {
            if (mount_table[i].fs == fs) {
                int plen = kstrlen(mount_table[i].prefix);
                if (kstrncmp(path, mount_table[i].prefix, (size_t)plen) == 0) {
                    inner_path += plen;
                    while (*inner_path == '/') inner_path++;
                    if (*inner_path == '\0') inner_path = "/";
                    break;
                }
            }
        }

        /* if we have a mounted filesystem that provides its own find, delegate */
        if (fs && fs->ops && fs->ops->readdir) {
            /* Use generic path walking via readdir */
            if (inner_path[0] == '/' && inner_path[1] == '\0')
                return fs->root;

            const char* p = inner_path;
            while (*p == '/') p++;
            if (!*p) return fs->root;

            vfs_node_t* cur = fs->root;
            while (*p && cur) {
                const char* start = p;
                while (*p && *p != '/') p++;
                int len = (int)(p - start);
                if (len == 0) break;

                vfs_node_t* found = dentry_lookup(cur, start, len);
                if (!found) {
                    uint32_t idx = 0;
                    while (1) {
                        vfs_node_t* child = NULL;
                        if (cur->fs && cur->fs->ops && cur->fs->ops->readdir) {
                            int r = cur->fs->ops->readdir(cur, idx, &child);
                            if (r != 0 || !child) break;
                        } else {
                            break;
                        }
                        int match = 1;
                        for (int i = 0; i < len; i++) {
                            if (child->name[i] != start[i]) { match = 0; break; }
                        }
                        if (match && child->name[len] == '\0') {
                            found = child;
                            dentry_add(cur, child);
                            break;
                        }
                        idx++;
                    }
                }

                if (!found) {
                    return NULL;
                }

                /* Check if we should follow symlinks during path walk */
                if (follow && (found->flags & VFS_FLAG_SYMLINK)) {
                    /* Only follow if there are more path components, NOT at the final step */
                    /* (final-step following is handled below) */
                    char c = p[0];
                    while (c == '/') { p++; c = *p; }
                    if (c != '\0') {
                        /* There are more components - follow the symlink */
                        char link_target[256];
                        if (found->fs && found->fs->ops && found->fs->ops->readlink &&
                            found->fs->ops->readlink(found, link_target, sizeof(link_target)) == 0) {
                            int k = 0;
                            if (link_target[0] == '/') {
                                for (int i = 0; link_target[i] && k < 511; i++)
                                    pathbuf[k++] = link_target[i];
                            } else {
                                for (int i = 0; link_target[i] && k < 511; i++)
                                    pathbuf[k++] = link_target[i];
                            }
                            if (k > 0 && pathbuf[k-1] != '/')
                                pathbuf[k++] = '/';
                            while (*p && k < 511)
                                pathbuf[k++] = *p++;
                            pathbuf[k] = '\0';
                            path = pathbuf;
                            depth++;
                            found_symlink = 1;
                            break;
                        }
                        return NULL;
                    }
                }

                cur = found;
                while (*p == '/') p++;
            }

            if (found_symlink) continue;

            /* Final node: follow symlink only if follow is set */
            if (follow && cur && (cur->flags & VFS_FLAG_SYMLINK)) {
                char link_target[256];
                if (cur->fs && cur->fs->ops && cur->fs->ops->readlink &&
                    cur->fs->ops->readlink(cur, link_target, sizeof(link_target)) == 0) {
                    kstrncpy(pathbuf, link_target, 511);
                    path = pathbuf;
                    depth++;
                    continue;
                }
                return NULL;
            }

            return cur;
        }

        /* Legacy: flat node list fallback */
        if (!fs || !fs->root) return NULL;

        if (inner_path[0] == '/' && inner_path[1] == '\0')
            return fs->root;

        vfs_node_t* cur = fs->root;
        const char* p = inner_path;
        while (*p == '/') p++;

        while (*p && cur) {
            const char* start = p;
            while (*p && *p != '/') p++;
            int len = (int)(p - start);
            if (len == 0) break;

            if (!cur->children) return NULL;
            vfs_node_t* child = cur->children;
            while (child) {
                int match = 1;
                for (int i = 0; i < len; i++) {
                    if (child->name[i] != start[i]) { match = 0; break; }
                }
                if (match && child->name[len] == '\0')
                    break;
                child = child->next;
            }

            if (!child) return NULL;

            if (follow && (child->flags & VFS_FLAG_SYMLINK)) {
                char link_target[256];
                if (child->fs && child->fs->ops && child->fs->ops->readlink &&
                    child->fs->ops->readlink(child, link_target, sizeof(link_target)) == 0) {
                    kstrncpy(pathbuf, link_target, 511);
                    path = pathbuf;
                    depth++;
                    found_symlink = 1;
                    break;
                }
                return NULL;
            }

            cur = child;
            while (*p == '/') p++;
        }

        if (found_symlink) continue;

        /* Final node symlink check for legacy path */
        if (follow && cur && (cur->flags & VFS_FLAG_SYMLINK)) {
            char link_target[256];
            if (cur->fs && cur->fs->ops && cur->fs->ops->readlink &&
                cur->fs->ops->readlink(cur, link_target, sizeof(link_target)) == 0) {
                kstrncpy(pathbuf, link_target, 511);
                path = pathbuf;
                depth++;
                continue;
            }
            return NULL;
        }

        return cur;
    }

    return NULL; /* Too many symlink levels */
}

vfs_node_t* vfs_find(const char* path) {
    return vfs_find_flags(path, 1);
}

vfs_node_t* vfs_find_nofollow(const char* path) {
    return vfs_find_flags(path, 0);
}

/* Check if current process can access a file with the given node.
 * Returns 0 on success, -1 on denial. */
int vfs_access_check(vfs_node_t* node, int want_write) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    /* Kernel threads with no process can access everything */
    if (!proc) return 0;
    /* Root (euid 0) can access everything */
    if (proc->euid == 0) return 0;
    /* CAP_DAC_OVERRIDE bypasses permission checks */
    if (proc->caps & CAP_DAC_OVERRIDE) return 0;

    /* Stat the node to get mode, uid, gid */
    vfs_stat_t st;
    int has_stat = 0;
    if (node->fs && node->fs->ops && node->fs->ops->stat) {
        if (node->fs->ops->stat(node, &st) == 0) has_stat = 1;
    }
    if (!has_stat) {
        st.mode = node->mode;
        st.uid  = node->uid;
        st.gid  = node->gid;
    }
    /* mode=0 means not set — allow for backward compat */
    if (st.mode == 0) return 0;

    uint32_t mode = st.mode & 0xFFFF; /* lower 16 bits are permissions */

    /* Determine which set of permission bits to use */
    uint32_t check_bits;
    if (proc->euid == st.uid)
        check_bits = (mode >> 6) & 7;  /* owner: bits 8-6 */
    else if (proc->egid == st.gid)
        check_bits = (mode >> 3) & 7;  /* group: bits 5-3 */
    else
        check_bits = mode & 7;         /* other: bits 2-0 */

    if (want_write) {
        if (!(check_bits & 2)) return -1;  /* write bit not set */
    } else {
        if (!(check_bits & 4)) return -1;  /* read bit not set */
    }
    return 0;
}

int vfs_open(const char* path, int flags) {
    /* Handle O_CREAT: create file if it doesn't exist */
    if (flags & O_CREAT) {
        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {
            int cr = vfs_create(path, 0);
            if (cr < 0) { kprintf("[VFS_OPEN] create '%s' failed\n", path); return -1; }
        }
    }

    vfs_node_t* node = vfs_find(path);
    if (!node) { kprintf("[VFS_OPEN] vfs_find '%s' failed\n", path); return -1; }

    /* DAC permission check */
    if (flags & (O_WRONLY | O_RDWR)) {
        if (vfs_access_check(node, 1) != 0) return -1;
    } else {
        if (vfs_access_check(node, 0) != 0) return -1;
    }

    /* Advisory write lock check */
    if ((flags & (O_WRONLY | O_RDWR)) && node->fs && node->fs->ops && node->fs->ops->stat) {
        vfs_stat_t st;
        if (node->fs->ops->stat(node, &st) == 0) {
            if (st.fs_flags & VFS_FS_FLAG_LOCKED) {
                kprintf("[VFS_OPEN] '%s' locked\n", path);
                return -1;
            }
        }
    }

    /* Handle O_TRUNC: truncate file to zero length */
    if ((flags & O_TRUNC) && node->fs && node->fs->ops && node->fs->ops->truncate)
        node->fs->ops->truncate(node, 0);

    if (node->fs && node->fs->ops && node->fs->ops->open)
        if (node->fs->ops->open(node) != 0) {
            kprintf("[VFS_OPEN] ops->open '%s' failed\n", path);
            return -1;
        }

    int fd = -1;
    {
        cpu_flags_t _sf;
        spinlock_acquire(&vfs_global_lock, &_sf);
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (!vfs_get_fd_table()[i].used) {
                vfs_get_fd_table()[i].node   = node;
                vfs_get_fd_table()[i].offset = 0;
                vfs_get_fd_table()[i].flags  = flags;
                vfs_get_fd_table()[i].used   = 1;
                __sync_fetch_and_add(&node->refcount, 1);
                fd = i;
                break;
            }
        }
        spinlock_release(&vfs_global_lock, _sf);
    }
    return fd;
}

int vfs_close(int fd) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_node_t* node = vfs_get_fd_table()[fd].node;
    vfs_get_fd_table()[fd].used = 0;
    spinlock_release(&vfs_global_lock, _sf);

    /* Notify the file system that the fd is being closed (signals EOF etc.).
     * This is called on EVERY close, not just the last reference, so pipe
     * can set write_closed/read_closed even when a child process drops
     * its inherited copy.  The node itself is only freed on last ref.
     */
    if (node->fs && node->fs->ops && node->fs->ops->close)
        node->fs->ops->close(node);

    if (__sync_fetch_and_sub(&node->refcount, 1) == 1) {
        if (node->dynamic) {
            if (node->destructor)
                node->destructor(node->private_data);
            kfree(node);
        }
    }

    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t count) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);

    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_fd_t* f = &vfs_get_fd_table()[fd];
    if ((f->flags & 3) == O_WRONLY) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_node_t* node = f->node;
    uint64_t cur_offset = f->offset;
    spinlock_release(&vfs_global_lock, _sf);

    if (node->fs && node->fs->ops && node->fs->ops->read) {
        int64_t ret = node->fs->ops->read(node, buf, count, cur_offset);
        if (ret > 0) {
            spinlock_acquire(&vfs_global_lock, &_sf);
            f->offset += ret;
            spinlock_release(&vfs_global_lock, _sf);
        }
        return ret;
    }
    return -1;
}

int64_t vfs_write(int fd, const void* buf, uint64_t count) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);

    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_fd_t* f = &vfs_get_fd_table()[fd];
    if ((f->flags & 3) == O_RDONLY) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_node_t* node = f->node;
    uint64_t cur_offset = f->offset;
    if (f->flags & O_APPEND) {
        cur_offset = node->size;
        f->offset = cur_offset;
    }
    spinlock_release(&vfs_global_lock, _sf);

    if (node->fs && node->fs->ops && node->fs->ops->write) {
        int64_t ret = node->fs->ops->write(node, buf, count, cur_offset);
        if (ret > 0) {
            spinlock_acquire(&vfs_global_lock, &_sf);
            f->offset = cur_offset + ret;
            spinlock_release(&vfs_global_lock, _sf);
        }
        return ret;
    }
    return -1;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);

    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }

    vfs_fd_t* f = &vfs_get_fd_table()[fd];
    vfs_node_t* node = f->node;

    switch (whence) {
        case VFS_SEEK_SET: f->offset = (uint64_t)offset; break;
        case VFS_SEEK_CUR: f->offset += offset; break;
        case VFS_SEEK_END: f->offset = node->size + offset; break;
        default: spinlock_release(&vfs_global_lock, _sf); return -1;
    }
    int64_t result = (int64_t)f->offset;
    spinlock_release(&vfs_global_lock, _sf);
    return result;
}

int vfs_create(const char* path, int is_dir) {
    if (!path) return -1;

    const char* slash = NULL;
    const char* p = path;
    while (*p) {
        if (*p == '/') slash = p;
        p++;
    }

    const char* name;
    vfs_node_t* parent;
    if (slash) {
        int parent_len = (int)(slash - path);
        if (parent_len == 0) {
            parent = vfs_find("/");
        } else {
            char parent_path[256];
            int clen = parent_len < 255 ? parent_len : 255;
            kmemcpy(parent_path, path, (size_t)clen);
            parent_path[clen] = '\0';
            parent = vfs_find(parent_path);
        }
        name = slash + 1;
    } else {
        parent = vfs_find("/");
        name = path;
    }

    if (!parent) return -1;
    if (kstrlen(name) >= VFS_MAX_NAME) return -1;
    if (parent->fs && parent->fs->ops && parent->fs->ops->create)
        return parent->fs->ops->create(parent, name, is_dir);
    return -1;
}

int vfs_mkdir(const char* path) {
    return vfs_create(path, 1);
}

int vfs_rmdir(const char* path) {
    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;
    if (!(node->flags & 1)) return -1;

    /* Check directory is empty */
    if (node->fs && node->fs->ops && node->fs->ops->readdir) {
        vfs_node_t* child = NULL;
        if (node->fs->ops->readdir(node, 0, &child) == 0)
            return -1;
    }

    return vfs_unlink(path);
}

int vfs_ftruncate(int fd, uint64_t size) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }
    vfs_node_t* node = vfs_get_fd_table()[fd].node;
    spinlock_release(&vfs_global_lock, _sf);
    if (node->fs && node->fs->ops && node->fs->ops->truncate)
        return node->fs->ops->truncate(node, size);
    return -1;
}

int vfs_stat(const char* path, vfs_stat_t* st) {
    if (!path || !st) return -1;
    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;
    if (node->fs && node->fs->ops && node->fs->ops->stat)
        return node->fs->ops->stat(node, st);
    st->size  = node->size;
    st->inode = node->inode;
    st->mode  = node->flags;
    st->flags = node->flags;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    st->fs_flags = 0;
    st->uid  = node->uid;
    st->gid  = node->gid;
    return 0;
}

int vfs_chmod(const char* path, uint32_t mode) {
    if (!path) return -1;
    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;
    if (node->fs && node->fs->ops && node->fs->ops->chmod)
        return node->fs->ops->chmod(node, mode);
    return -1;
}

int vfs_lock(const char* path) {
    if (!path) return -1;
    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;
    if (node->fs && node->fs->ops && node->fs->ops->lock)
        return node->fs->ops->lock(node);
    return -1;
}

int vfs_unlock(const char* path) {
    if (!path) return -1;
    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;
    if (node->fs && node->fs->ops && node->fs->ops->unlock)
        return node->fs->ops->unlock(node);
    return -1;
}

int vfs_unlink(const char* path) {
    if (!path) return -1;

    const char* slash = NULL;
    const char* p = path;
    while (*p) {
        if (*p == '/') slash = p;
        p++;
    }

    const char* name;
    vfs_node_t* parent;
    if (slash) {
        int parent_len = (int)(slash - path);
        if (parent_len == 0) {
            parent = vfs_find("/");
        } else {
            char parent_path[256];
            int clen = parent_len < 255 ? parent_len : 255;
            kmemcpy(parent_path, path, (size_t)clen);
            parent_path[clen] = '\0';
            parent = vfs_find(parent_path);
        }
        name = slash + 1;
    } else {
        parent = vfs_find("/");
        name = path;
    }

    if (!parent) return -1;
    if (parent->fs && parent->fs->ops && parent->fs->ops->unlink)
        return parent->fs->ops->unlink(parent, name);
    return -1;
}

/* Helper: parse a path into (parent_dir, basename) */
static int split_path(const char* path, vfs_node_t** parent_out, const char** name_out) {
    if (!path) return -1;
    const char* slash = NULL;
    for (const char* p = path; *p; p++)
        if (*p == '/') slash = p;
    if (slash) {
        int parent_len = (int)(slash - path);
        if (parent_len == 0) {
            *parent_out = vfs_find("/");
        } else {
            char parent_path[256];
            int clen = parent_len < 255 ? parent_len : 255;
            kmemcpy(parent_path, path, (size_t)clen);
            parent_path[clen] = '\0';
            *parent_out = vfs_find(parent_path);
        }
        *name_out = slash + 1;
    } else {
        *parent_out = vfs_find("/");
        *name_out = path;
    }
    return (*parent_out) ? 0 : -1;
}

int vfs_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -1;
    vfs_node_t* old_parent;
    vfs_node_t* new_parent;
    const char* old_name;
    const char* new_name;
    if (split_path(oldpath, &old_parent, &old_name) < 0) return -1;
    if (split_path(newpath, &new_parent, &new_name) < 0) return -1;
    if (old_parent->fs && old_parent->fs->ops && old_parent->fs->ops->rename)
        return old_parent->fs->ops->rename(old_parent, old_name, new_parent, new_name);
    return -1;
}

int vfs_link(const char* target, const char* linkpath) {
    if (!target || !linkpath) return -1;
    vfs_node_t* target_node = vfs_find(target);
    if (!target_node) return -1;
    vfs_node_t* parent;
    const char* name;
    if (split_path(linkpath, &parent, &name) < 0) return -1;
    if (parent->fs && parent->fs->ops && parent->fs->ops->link)
        return parent->fs->ops->link(parent, name, target_node);
    return -1;
}

int vfs_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath) return -1;
    vfs_node_t* parent;
    const char* name;
    if (split_path(linkpath, &parent, &name) < 0) return -1;
    if (parent->fs && parent->fs->ops && parent->fs->ops->symlink)
        return parent->fs->ops->symlink(parent, name, target);
    return -1;
}

int vfs_readlink(const char* path, char* buf, uint64_t size) {
    if (!path || !buf) return -1;
    vfs_node_t* node = vfs_find_nofollow(path);
    if (!node) return -1;
    if (node->fs && node->fs->ops && node->fs->ops->readlink)
        return node->fs->ops->readlink(node, buf, size);
    return -1;
}

int vfs_poll(int fd, int events, int* revents) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        *revents = POLLNVAL;
        return -1;
    }
    vfs_node_t* node = vfs_get_fd_table()[fd].node;
    spinlock_release(&vfs_global_lock, _sf);
    if (node->fs && node->fs->ops && node->fs->ops->poll) {
        return node->fs->ops->poll(node, events, revents);
    }
    /* Default: always writable, never readable */
    *revents = (events & POLLOUT) ? POLLOUT : 0;
    return 0;
}

int vfs_ioctl(int fd, uint64_t request, void* argp) {
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    if (fd < 0 || fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return -1;
    }
    vfs_node_t* node = vfs_get_fd_table()[fd].node;
    spinlock_release(&vfs_global_lock, _sf);
    if (node->fs && node->fs->ops && node->fs->ops->ioctl)
        return node->fs->ops->ioctl(node, request, argp);
    return -1;
}
