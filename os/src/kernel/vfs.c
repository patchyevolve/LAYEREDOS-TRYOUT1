#include "kernel.h"
#include "vfs.h"

vfs_fd_t  fd_table[VFS_MAX_FDS];
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
    kmemset(&root_node, 0, sizeof(root_node));
    kstrncpy(root_node.name, "/", VFS_MAX_NAME - 1);
    root_node.flags = 1;

    for (int i = 0; i < VFS_MAX_FDS; i++)
        fd_table[i].used = 0;

    for (int i = 0; i < 3; i++)
        fd_table[i].used = 1;

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

static vfs_node_t* vfs_find_flags(const char* path, int follow) {
    if (!path) return NULL;

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

                vfs_node_t* found = NULL;
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
                        break;
                    }
                    idx++;
                }

                if (!found) return NULL;

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
                            char newpath[512];
                            int k = 0;
                            if (link_target[0] == '/') {
                                for (int i = 0; link_target[i] && k < 511; i++)
                                    newpath[k++] = link_target[i];
                            } else {
                                for (int i = 0; link_target[i] && k < 511; i++)
                                    newpath[k++] = link_target[i];
                            }
                            if (k > 0 && newpath[k-1] != '/')
                                newpath[k++] = '/';
                            while (*p && k < 511)
                                newpath[k++] = *p++;
                            newpath[k] = '\0';
                            path = newpath;
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
                    path = link_target;
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
                    path = link_target;
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
                path = link_target;
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

int vfs_open(const char* path, int flags) {
    /* Handle O_CREAT: create file if it doesn't exist */
    if (flags & O_CREAT) {
        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {
            if (vfs_create(path, 0) < 0) return -1;
        }
    }

    vfs_node_t* node = vfs_find(path);
    if (!node) return -1;

    /* Check write permission when opening for write */
    if ((flags & (O_WRONLY | O_RDWR)) && node->fs && node->fs->ops && node->fs->ops->stat) {
        vfs_stat_t st;
        if (node->fs->ops->stat(node, &st) == 0) {
            /* mode=0 means unset (backward compat); reject if explicitly read-only */
            if (st.mode != 0 && (st.mode & 0200) == 0)
                return -1;
            /* Advisory write lock check */
            if (st.fs_flags & VFS_FS_FLAG_LOCKED)
                return -1;
        }
    }

    int fd = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    /* Handle O_TRUNC: truncate file to zero length */
    if ((flags & O_TRUNC) && node->fs && node->fs->ops && node->fs->ops->truncate)
        node->fs->ops->truncate(node, 0);

    if (node->fs && node->fs->ops && node->fs->ops->open)
        node->fs->ops->open(node);

    fd_table[fd].node   = node;
    fd_table[fd].offset = (flags & O_APPEND) ? node->size : 0;
    fd_table[fd].flags  = flags;
    fd_table[fd].used   = 1;
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -1;

    vfs_node_t* node = fd_table[fd].node;
    if (node->fs && node->fs->ops && node->fs->ops->close)
        node->fs->ops->close(node);

    fd_table[fd].used = 0;
    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -1;

    vfs_fd_t* f = &fd_table[fd];
    vfs_node_t* node = f->node;

    if (node->fs && node->fs->ops && node->fs->ops->read) {
        int64_t ret = node->fs->ops->read(node, buf, count, f->offset);
        if (ret > 0) f->offset += ret;
        return ret;
    }
    return -1;
}

int64_t vfs_write(int fd, const void* buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -1;

    vfs_fd_t* f = &fd_table[fd];
    vfs_node_t* node = f->node;

    if (node->fs && node->fs->ops && node->fs->ops->write) {
        if (f->flags & O_APPEND) f->offset = node->size;
        int64_t ret = node->fs->ops->write(node, buf, count, f->offset);
        if (ret > 0) f->offset += ret;
        return ret;
    }
    return -1;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -1;

    vfs_fd_t* f = &fd_table[fd];
    vfs_node_t* node = f->node;

    switch (whence) {
        case VFS_SEEK_SET: f->offset = (uint64_t)offset; break;
        case VFS_SEEK_CUR: f->offset += offset; break;
        case VFS_SEEK_END: f->offset = node->size + offset; break;
        default: return -1;
    }
    return (int64_t)f->offset;
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
    return vfs_unlink(path);
}

int vfs_ftruncate(int fd, uint64_t size) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -1;
    vfs_node_t* node = fd_table[fd].node;
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
