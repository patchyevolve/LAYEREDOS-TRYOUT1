#include "kernel.h"
#include "procfs.h"
#include "vfs.h"
#include "pmm.h"
#include "hal.h"
#include "smp.h"
#include "process.h"
#include "vma.h"
#include "sched.h"
#include "kmalloc.h"

enum procfs_type {
    PROCFS_ROOT,
    PROCFS_CPUINFO,
    PROCFS_MEMINFO,
    PROCFS_UPTIME,
    PROCFS_VERSION,
    PROCFS_STAT,
    PROCFS_SELF,
    PROCFS_PID_DIR,
    PROCFS_PID_STATUS,
    PROCFS_PID_MAPS,
    PROCFS_PID_CMDLINE,
};

typedef struct {
    int   type;
    pid_t pid;
} procfs_info_t;

#define PROC_STATIC_FILES 6
static const char* proc_fnames[PROC_STATIC_FILES] = {
    "cpuinfo", "meminfo", "uptime", "version", "stat", "self"
};
static const int proc_ftypes[PROC_STATIC_FILES] = {
    PROCFS_CPUINFO, PROCFS_MEMINFO, PROCFS_UPTIME,
    PROCFS_VERSION, PROCFS_STAT, PROCFS_SELF
};

#define PID_ENTRIES 3
static const char* pid_fnames[PID_ENTRIES] = {
    "status", "maps", "cmdline"
};
static const int pid_ftypes[PID_ENTRIES] = {
    PROCFS_PID_STATUS, PROCFS_PID_MAPS, PROCFS_PID_CMDLINE
};

static int fmt(char* buf, int sz, const char* f, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = kvsnprintf(buf, (size_t)sz, f, ap);
    __builtin_va_end(ap);
    return n;
}

static int gen_cpuinfo(char* b, int sz) {
    int nr = smp_nr_cpus();
    int off = 0;
    for (int i = 0; i < nr && off < sz - 64; i++)
        off += fmt(b + off, sz - off, "processor\t: %d\n", i);
    return off;
}

static int gen_meminfo(char* b, int sz) {
    uint64_t total_pg = pmm_total_pages();
    uint64_t free_pg  = pmm_free_pages_count();
    uint64_t total_kb = total_pg * 4;
    uint64_t free_kb  = free_pg * 4;
    uint64_t used_kb  = total_kb - free_kb;
    return fmt(b, sz, "MemTotal:\t%8lu kB\nMemFree:\t%8lu kB\nMemUsed:\t%8lu kB\n",
               total_kb, free_kb, used_kb);
}

static int gen_uptime(char* b, int sz) {
    uint64_t ticks = hal_timer_get_ticks();
    uint64_t hz    = hal_timer_get_hz();
    uint64_t secs  = ticks / hz;
    uint64_t frac  = (ticks % hz) * 100 / hz;
    return fmt(b, sz, "%lu.%02lu\n", secs, frac);
}

static int gen_version(char* b, int sz) {
    return fmt(b, sz, "OPERtur/TRY1 OS v0.2.0 (x86-64, %d CPU)\n", smp_nr_cpus());
}

static int gen_stat(char* b, int sz) {
    int off = 0;
    int nr = smp_nr_cpus();
    uint64_t total_cs = 0, total_irq = 0;
    for (int i = 0; i < nr; i++) {
        per_cpu_data_t* pcp = per_cpu_data[i];
        if (!pcp) continue;
        uint64_t cs  = pcp->context_switches;
        uint64_t irq = pcp->irq_count;
        off += fmt(b + off, sz - off, "cpu%d %lu %lu\n", i, cs, irq);
        total_cs  += cs;
        total_irq += irq;
    }
    off += fmt(b + off, sz - off, "intr %lu\nctxt %lu\n", total_irq, total_cs);
    return off;
}

static int gen_pid_status(char* b, int sz, pid_t pid) {
    process_t* p = process_find(pid);
    if (!p) return fmt(b, sz, "Pid:\t%lu\nState:\tdead\n", pid);
    const char* state = p->exited ? "Z (zombie)" : "R (running)";
    return fmt(b, sz,
        "Name:\t%s\nPid:\t%lu\nPPid:\t%lu\nState:\t%s\n"
        "Uid:\t%lu\t%lu\t%lu\t%lu\n"
        "Gid:\t%lu\t%lu\t%lu\t%lu\n"
        "Threads:\t%d\n"
        "VmSize:\t%lu kB\n",
        p->name, p->pid, p->ppid, state,
        p->uid, p->euid, p->gid, p->egid,
        p->uid, p->euid, p->gid, p->egid,
        p->thread_count,
        (p->user_code_size + 4095) / 1024);
}

static int gen_pid_maps(char* b, int sz, pid_t pid) {
    process_t* p = process_find(pid);
    if (!p) return 0;
    int off = 0;
    vma_t* v = (vma_t*)p->vmas;
    while (v && off < sz - 80) {
        char perm[4] = { '-', '-', '-', '\0' };
        if (v->prot & 1) perm[0] = 'r';
        if (v->prot & 2) perm[1] = 'w';
        if (v->prot & 4) perm[2] = 'x';
        const char* path = "";
        if (v->node) path = v->node->name;
        off += fmt(b + off, sz - off, "%016lx-%016lx %s %s\n",
                   v->start, v->end, perm, path);
        v = v->next;
    }
    return off;
}

static int gen_pid_cmdline(char* b, int sz, pid_t pid) {
    process_t* p = process_find(pid);
    if (!p) return 0;
    return fmt(b, sz, "%s\n", p->name);
}

static int gen_content(char* buf, int sz, int type, pid_t pid) {
    switch (type) {
        case PROCFS_CPUINFO:    return gen_cpuinfo(buf, sz);
        case PROCFS_MEMINFO:    return gen_meminfo(buf, sz);
        case PROCFS_UPTIME:     return gen_uptime(buf, sz);
        case PROCFS_VERSION:    return gen_version(buf, sz);
        case PROCFS_STAT:       return gen_stat(buf, sz);
        case PROCFS_PID_STATUS: return gen_pid_status(buf, sz, pid);
        case PROCFS_PID_MAPS:   return gen_pid_maps(buf, sz, pid);
        case PROCFS_PID_CMDLINE:return gen_pid_cmdline(buf, sz, pid);
        default: return 0;
    }
}

static vfs_node_t* alloc_child(vfs_fs_t* fs, const char* name, int flags,
                                int type, pid_t pid)
{
    vfs_node_t* n = kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    kmemset(n, 0, sizeof(vfs_node_t));
    kstrncpy(n->name, name, VFS_MAX_NAME - 1);
    n->flags = flags;
    n->fs = fs;
    procfs_info_t* info = kmalloc(sizeof(procfs_info_t));
    if (!info) { kfree(n); return NULL; }
    info->type = type;
    info->pid  = pid;
    n->private_data = info;
    n->uid = 0;
    n->gid = 0;
    return n;
}

static int procfs_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int procfs_vfs_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int64_t procfs_vfs_read(vfs_node_t* node, void* buf,
                                uint64_t count, uint64_t offset)
{
    procfs_info_t* info = (procfs_info_t*)node->private_data;
    if (!info) return -1;
    char tmp[4096];
    int len = gen_content(tmp, sizeof(tmp), info->type, info->pid);
    if (len < 0) return -1;
    if ((int64_t)offset >= len) return 0;
    uint64_t avail = (uint64_t)len - offset;
    if (count > avail) count = avail;
    kmemcpy(buf, tmp + offset, count);
    return (int64_t)count;
}

static int64_t procfs_vfs_write(vfs_node_t* node, const void* buf,
                                 uint64_t count, uint64_t offset)
{
    (void)node; (void)buf; (void)count; (void)offset;
    return -1;
}

struct cb_ctx {
    vfs_fs_t* fs;
    int       skip;
    vfs_node_t** out;
};

static void pid_dir_cb(process_t* p, void* arg) {
    struct cb_ctx* ctx = (struct cb_ctx*)arg;
    if (*ctx->out) return;
    if (ctx->skip > 0) { ctx->skip--; return; }
    char pname[16];
    fmt(pname, sizeof(pname), "%lu", p->pid);
    *ctx->out = alloc_child(ctx->fs, pname, 1, PROCFS_PID_DIR, p->pid);
}

static int procfs_vfs_readdir_root(vfs_node_t* node, uint32_t index,
                                    vfs_node_t** out)
{
    if (index < PROC_STATIC_FILES) {
        int type = proc_ftypes[index];
        const char* name = proc_fnames[index];
        int flags = (type == PROCFS_SELF) ? VFS_FLAG_SYMLINK : 0;
        *out = alloc_child(node->fs, name, flags, type, 0);
        return *out ? 0 : -1;
    }
    uint32_t pid_idx = index - PROC_STATIC_FILES;
    struct cb_ctx ctx;
    ctx.fs   = node->fs;
    ctx.skip = (int)pid_idx;
    ctx.out  = out;
    *out = NULL;
    process_iterate(pid_dir_cb, &ctx);
    return *out ? 0 : -1;
}

static int procfs_vfs_readdir_pid(vfs_node_t* node, uint32_t index,
                                   vfs_node_t** out)
{
    procfs_info_t* info = (procfs_info_t*)node->private_data;
    if (!info) return -1;
    if (index < PID_ENTRIES) {
        int flags = 0;
        *out = alloc_child(node->fs, pid_fnames[index], flags,
                           pid_ftypes[index], info->pid);
        return *out ? 0 : -1;
    }
    return -1;
}

static int procfs_vfs_readdir(vfs_node_t* node, uint32_t index,
                               vfs_node_t** out)
{
    procfs_info_t* info = (procfs_info_t*)node->private_data;
    if (!info) return -1;
    switch (info->type) {
        case PROCFS_ROOT:    return procfs_vfs_readdir_root(node, index, out);
        case PROCFS_PID_DIR: return procfs_vfs_readdir_pid(node, index, out);
        default: return -1;
    }
}

static int procfs_vfs_create(vfs_node_t* dir, const char* name, int is_dir) {
    (void)dir; (void)name; (void)is_dir;
    return -1;
}

static int procfs_vfs_unlink(vfs_node_t* dir, const char* name) {
    (void)dir; (void)name;
    return -1;
}

static int procfs_vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    procfs_info_t* info = (procfs_info_t*)node->private_data;
    st->size  = 0;
    st->inode = (uint64_t)(uintptr_t)info;
    st->mode  = 0444;
    if (node->flags & 1) st->mode = 0555;
    st->flags = node->flags;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    st->fs_flags = 0;
    st->uid  = node->uid;
    st->gid  = node->gid;
    return 0;
}

static int procfs_vfs_truncate(vfs_node_t* node, uint64_t size) {
    (void)node; (void)size;
    return 0;
}

static int procfs_vfs_chmod(vfs_node_t* node, uint32_t mode) {
    (void)node; (void)mode;
    return 0;
}

static int procfs_vfs_lock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int procfs_vfs_unlock(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int procfs_vfs_rename(vfs_node_t* old_dir, const char* old_name,
                              vfs_node_t* new_dir, const char* new_name)
{
    (void)old_dir; (void)old_name; (void)new_dir; (void)new_name;
    return -1;
}

static int procfs_vfs_link(vfs_node_t* dir, const char* name, vfs_node_t* target) {
    (void)dir; (void)name; (void)target;
    return -1;
}

static int procfs_vfs_symlink(vfs_node_t* dir, const char* name, const char* target) {
    (void)dir; (void)name; (void)target;
    return -1;
}

static int procfs_vfs_readlink(vfs_node_t* node, char* buf, uint64_t size) {
    procfs_info_t* info = (procfs_info_t*)node->private_data;
    if (!info || info->type != PROCFS_SELF) return -1;
    pid_t pid = process_get_current_pid();
    int len = fmt(buf, (int)size, "/proc/%lu", pid);
    if (len < 0) return -1;
    return len;
}

static int procfs_vfs_ioctl(vfs_node_t* node, uint64_t request, void* argp) {
    (void)node; (void)request; (void)argp;
    return -1;
}

static vfs_file_ops_t procfs_ops = {
    .open     = procfs_vfs_open,
    .close    = procfs_vfs_close,
    .read     = procfs_vfs_read,
    .write    = procfs_vfs_write,
    .readdir  = procfs_vfs_readdir,
    .create   = procfs_vfs_create,
    .unlink   = procfs_vfs_unlink,
    .rename   = procfs_vfs_rename,
    .link     = procfs_vfs_link,
    .stat     = procfs_vfs_stat,
    .truncate = procfs_vfs_truncate,
    .chmod    = procfs_vfs_chmod,
    .lock     = procfs_vfs_lock,
    .unlock   = procfs_vfs_unlock,
    .symlink  = procfs_vfs_symlink,
    .readlink = procfs_vfs_readlink,
    .ioctl    = procfs_vfs_ioctl,
};

typedef struct {
    vfs_fs_t vfs_fs;
    vfs_node_t root_node;
    procfs_info_t root_info;
} procfs_fs_t;

static procfs_fs_t* procfs_fs = NULL;

err_t procfs_mount(void) {
    procfs_fs = kmalloc(sizeof(procfs_fs_t));
    if (!procfs_fs) return ERR_NOMEM;
    kmemset(procfs_fs, 0, sizeof(procfs_fs_t));

    kstrncpy(procfs_fs->vfs_fs.name, "procfs", sizeof(procfs_fs->vfs_fs.name) - 1);
    procfs_fs->vfs_fs.root = &procfs_fs->root_node;
    procfs_fs->vfs_fs.ops = &procfs_ops;

    procfs_fs->root_info.type = PROCFS_ROOT;
    procfs_fs->root_info.pid  = 0;

    kmemset(&procfs_fs->root_node, 0, sizeof(procfs_fs->root_node));
    kstrncpy(procfs_fs->root_node.name, "/", VFS_MAX_NAME - 1);
    procfs_fs->root_node.flags = 1;
    procfs_fs->root_node.size = 0;
    procfs_fs->root_node.fs = &procfs_fs->vfs_fs;
    procfs_fs->root_node.private_data = &procfs_fs->root_info;

    err_t e = vfs_mount("/proc", &procfs_fs->vfs_fs);
    if (e) { kfree(procfs_fs); procfs_fs = NULL; return e; }

    kprintf("[PROCFS] Mounted at /proc\n");
    return ERR_OK;
}
