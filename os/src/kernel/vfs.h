#ifndef VFS_H
#define VFS_H

#include "types.h"

#define VFS_MAX_NAME   64
#define VFS_MAX_FILES  64
#define VFS_MAX_FDS    128
#define VFS_SEEK_SET   0
#define VFS_SEEK_CUR   1
#define VFS_SEEK_END   2

/* Open flags (POSIX-ish) */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_NONBLOCK 04000

/* FS flags returned by stat */
#define VFS_FS_FLAG_LOCKED 1

/* Node flags */
#define VFS_FLAG_SYMLINK 2

typedef struct vfs_node vfs_node_t;
typedef struct vfs_fs   vfs_fs_t;

typedef struct vfs_node {
    char        name[VFS_MAX_NAME];
    uint32_t    flags;
    uint64_t    size;
    uint64_t    inode;
    vfs_node_t* parent;
    vfs_node_t* children;
    vfs_node_t* next;
    vfs_fs_t*   fs;
    void*       private_data;
    int         refcount;
    int         dynamic;    /* non-zero → vfs_close kfrees node when refcount hits 0 */
    void        (*destructor)(void* private_data); /* called before kfree when dynamic */
} vfs_node_t;

typedef struct vfs_stat {
    uint64_t    size;
    uint64_t    inode;
    uint32_t    mode;
    uint32_t    flags;
    uint64_t    atime;
    uint64_t    mtime;
    uint64_t    ctime;
    uint32_t    fs_flags;
} vfs_stat_t;

typedef struct vfs_file_ops {
    int     (*open)(vfs_node_t* node);
    int     (*close)(vfs_node_t* node);
    int64_t (*read)(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset);
    int64_t (*write)(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset);
    int     (*readdir)(vfs_node_t* node, uint32_t index, vfs_node_t** out);
    int     (*create)(vfs_node_t* dir, const char* name, int is_dir);
    int     (*unlink)(vfs_node_t* dir, const char* name);
    int     (*link)(vfs_node_t* dir, const char* name, vfs_node_t* target);
    int     (*rename)(vfs_node_t* old_dir, const char* old_name, vfs_node_t* new_dir, const char* new_name);
    int     (*stat)(vfs_node_t* node, vfs_stat_t* st);
    int     (*truncate)(vfs_node_t* node, uint64_t size);
    int     (*chmod)(vfs_node_t* node, uint32_t mode);
    int     (*lock)(vfs_node_t* node);
    int     (*unlock)(vfs_node_t* node);
    int     (*symlink)(vfs_node_t* dir, const char* name, const char* target);
    int     (*readlink)(vfs_node_t* node, char* buf, uint64_t size);
    int     (*ioctl)(vfs_node_t* node, uint64_t request, void* argp);
} vfs_file_ops_t;

typedef struct vfs_fs {
    char            name[16];
    vfs_node_t*     root;
    vfs_file_ops_t* ops;
} vfs_fs_t;

typedef struct vfs_fd {
    vfs_node_t* node;
    uint64_t    offset;
    int         flags;
    int         used;
} vfs_fd_t;

err_t vfs_init(void);
err_t vfs_mount(const char* path, vfs_fs_t* fs);
vfs_node_t* vfs_find(const char* path);
vfs_node_t* vfs_find_nofollow(const char* path);
int  vfs_open(const char* path, int flags);
int  vfs_close(int fd);
int64_t vfs_read(int fd, void* buf, uint64_t count);
int64_t vfs_write(int fd, const void* buf, uint64_t count);
int64_t vfs_lseek(int fd, int64_t offset, int whence);
int  vfs_register_fs(vfs_fs_t* fs);
int  vfs_create(const char* path, int is_dir);
int  vfs_unlink(const char* path);
int  vfs_link(const char* target, const char* linkpath);
int  vfs_rename(const char* oldpath, const char* newpath);
int  vfs_mkdir(const char* path);
int  vfs_rmdir(const char* path);
int  vfs_stat(const char* path, vfs_stat_t* st);
int  vfs_ftruncate(int fd, uint64_t size);
int  vfs_chmod(const char* path, uint32_t mode);
int  vfs_lock(const char* path);
int  vfs_unlock(const char* path);
int  vfs_symlink(const char* target, const char* linkpath);
int  vfs_readlink(const char* path, char* buf, uint64_t size);
int  vfs_ioctl(int fd, uint64_t request, void* argp);
vfs_fd_t* vfs_get_fd_table(void);

#endif
