#pragma once

#include "kernel.h"
#include "sched.h"
#include "signal.h"
#include "sync.h"
#include "vfs.h"

struct int_frame;

#define MAX_PROCESSES 256
#define PROCESS_NAME_MAX 64
#define MAX_FDS 128

typedef uint64_t pid_t;

/* Process flags */
#define PROC_FLAG_STOPPED 1

// Process control block
typedef struct process_t {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    char name[PROCESS_NAME_MAX];

    // Memory management
    uint64_t cr3; // Page table base
    uint64_t entry_point;
    uint64_t user_stack_top;
    uint64_t mmap_brk;  // next mmap hint (grows upward from 0x40000000)
    uint64_t user_code_start;
    uint64_t user_code_size;

    // Threads
    list_head_t threads;        /* head of per-process thread list */
    list_head_t process_node;   /* node in global process_list */
    int thread_count;

    // Exit status
    int exit_code;
    int exited;
    wait_queue_t exit_waiters;

    // Signals
    volatile uint64_t pending_signals;
    uint64_t blocked_signals;
    sigaction_t signal_actions[NSIG];
    uint32_t flags;
    spinlock_t signal_lock;

    // Working directory
    char cwd[256];

    // Virtual memory areas (mmap tracking)
    void* vmas;  /* singly-linked VMA list (vma_t) */
    spinlock_t vma_lock;

    // Network namespace
    struct net_ns* net_ns;

    // Security & capabilities
    uint64_t caps;                    /* capability bitmask (see security.h) */
    uint64_t uid, gid, euid, egid;    /* user/group identity */
    int no_new_privs;
    uint64_t syscall_mask[4];         /* 256-bit syscall filter mask */

    // Fork tracking
    int64_t fork_count;   /* number of forks this process has done */
    int64_t fork_limit;   /* -1 = unlimited (default), >=0 = hard limit */

    // File descriptors (per-process fd table)
    vfs_fd_t fds[MAX_FDS];
} process_t;

err_t process_init(void);
process_t* process_create(const char* name, pid_t ppid);
err_t process_exec(process_t* proc, const void* elf_data, size_t elf_len);
err_t process_exit(process_t* proc, int exit_code);
void process_reap(process_t* proc);
process_t* process_find(pid_t pid);
pid_t process_get_current_pid(void);
uint64_t aslr_rand(void);
void signal_send(pid_t pid, int sig);
void signal_process(process_t* proc);
void signal_send_pgid(pid_t pgid, int sig);
void signal_deliver_custom(process_t* proc, struct int_frame* frame);

/* waitpid options */
#define WNOHANG   1
#define WUNTRACED 2
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    (((s) >> 8) & 0xff)
#define W_STOPCODE(sig) ((sig) << 8 | 0x7f)
