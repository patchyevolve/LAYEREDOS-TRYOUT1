# OPERtur / TRY1 OS — Backend Schema

**Version:** 2.0  
**Scope:** All significant in-memory and on-disk data structures across every kernel subsystem. Each structure documents its fields, size constraints, ownership rules, and lifecycle. Structures are presented in dependency order (lower layers first).

---

## 1. Physical Memory Manager

### `pmm_state_t` (global singleton)
```c
typedef struct {
    uint8_t     *bitmap;            // 1 bit per 4 KiB frame; 1=free, 0=used
    uint64_t    total_frames;       // Total physical frames detected at boot
    uint64_t    free_frames;        // Updated on every alloc/free
    uint64_t    hint;               // Next frame to check (fast-path scan start)
    uint64_t    last_alloc_frame;   // Last allocated frame (contiguity hint)
    spinlock_t  lock;
} pmm_state_t;
```
**Invariants:**
- `bitmap[frame]` is set atomically under `lock`
- `hint` wraps at `total_frames`
- Poison pattern `0xDEADBEEFDEADBEEF` written to first 8 bytes of every freed frame (double-free detection)
- Corruption checks on alloc: alignment, index bounds, bitmap double-alloc detect

---

## 2. Virtual Memory Manager

### `vmm_pml4_t`
- Pointer to a physical page containing the 512-entry PML4 table (x86-64 4-level paging)
- One PML4 per process; stored as `uint64_t *pml4` in `process_t`
- Kernel mappings (above `0xFFFFFFFF80000000`) are duplicated in every PML4 at construction time
- PID stamp at `pml4[255]`: stores `proc->pid` for stale-PML4 detection at exit time
- Self-reference check: `vmm_free_user_pages` skips PML4 entries pointing to itself

### `page_flags_t` (bitmask, fits in uint64_t PTE)
```c
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_NX         (1ULL << 63)
#define PAGE_SWAP_MARKER (1ULL << 9)   // Present=0, this bit=1 → PTE encodes swap slot
```

### Swap PTE Encoding (non-present)
```
Bits 63:12 → swap slot number (up to 2^52 slots)
Bit  9     → PAGE_SWAP_MARKER (distinguishes from unmapped)
Bit  0     → 0 (not present)
```

---

## 3. Heap Allocator

### `slab_cache_t`
```c
typedef struct slab_cache {
    uint32_t    obj_size;           // Object size (16, 32, 64, 128, 256, 512, 1024)
    uint32_t    objs_per_slab;      // Objects per 4 KiB slab page
    void        *free_list;         // Singly-linked free list (first word of free obj = next ptr)
    uint64_t    alloc_count;
    uint64_t    free_count;
    uint32_t    page_count;         // Total slab pages allocated
    spinlock_t  lock;
} slab_cache_t;
```

### `buddy_block_t` (large alloc fallback)
```c
typedef struct buddy_block {
    uint32_t    order;              // Size = 4096 * 2^order
    bool        free;
    struct buddy_block *buddy;      // NULL if order == MAX_ORDER
} buddy_block_t;
```

### `kmalloc_compact()`
- Scans all slab caches
- For each page in a slab: if no objects allocated, free the page back to PMM
- Called from `pmm_oom_kill()` before killing the offending process

---

## 4. Thread Control Block

### `cpu_context_t`
```c
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs,  ss;
    uint8_t  fxsave_region[512];    // FXSAVE/FXRSTOR state (16-byte aligned)
} cpu_context_t;
```

### `thread_t`
```c
typedef struct thread {
    cpu_context_t       ctx;
    uint64_t            kernel_rsp;         // Kernel stack pointer when in user mode
    uint64_t            kernel_stack;       // Stack base address (4 pages = 16 KiB)
    uint64_t            kernel_stack_size;  // THREAD_STACK_SIZE = 16384
    int                 base_priority;      // 0 (highest) – 255 (lowest)
    int                 effective_priority; // After inheritance / aging
    uint64_t            sleep_until_ms;     // Wake time (ms); 0 = not sleeping
    wait_queue_t        *blocked_on;        // NULL or queue this thread is waiting on
    mutex_t             *held_mutex;        // Mutex currently owned (for priority inheritance)
    uint64_t            *pml4;              // Per-thread PML4 (0 for kernel threads)
    struct process      *proc;              // Owning process
    struct thread       *next;              // Scheduler run queue linkage
    char                name[32];
    enum {
        THREAD_READY,
        THREAD_RUNNING,
        THREAD_SLEEPING,
        THREAD_BLOCKED,
        THREAD_ZOMBIE
    } state;
} thread_t;
```
**Stack constraint:** 16 KiB total, no guard page. Stack overflow corrupts adjacent pages. All automatic variables must be well under 16 KB.

---

## 5. Process Descriptor

### `fd_entry_t`
```c
typedef struct {
    vnode_t     *vnode;
    uint64_t    offset;
    uint32_t    flags;          // O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK
    uint32_t    refcount;
} fd_entry_t;
```

### `fd_table_t`
```c
typedef struct {
    fd_entry_t  *entries[32];   // 32 FD slots; pre-allocated 0/1/2 = ttyS0
    spinlock_t  lock;
} fd_table_t;
```
Socket FDs: `sys_read`/`sys_write`/`sys_close` check `sock_lookup(fd)` before falling through to VFS.

### `signal_state_t`
```c
typedef struct {
    void        (*handlers[32])(int);   // sigaction handlers; NULL = SIG_DFL
    uint32_t    pending;                // Bitmask of pending signals
    uint32_t    blocked;                // Blocked mask (sigprocmask)
    uint64_t    trampoline_addr;        // SIGNAL_TRAMPOLINE_ADDR mapped in every process
} signal_state_t;
```

### `process_t`
```c
typedef struct process {
    pid_t               pid;
    pid_t               ppid;
    pid_t               pgid;           // Process group ID (TTY signal delivery)
    uint64_t            *pml4;          // Physical address of PML4 page (stamp at pml4[255])
    fd_table_t          *fd_table;
    signal_state_t      signals;
    thread_t            *thread_list;   // All threads; currently 1 per process
    int                 exit_code;
    // Planned Stage 6 additions:
    // uid_t            uid;
    // gid_t            gid;
    // cap_table_t      *caps;
    // syscall_policy_t *seccomp;
    enum {
        PROC_RUNNING,
        PROC_ZOMBIE,
        PROC_STOPPED
    } state;
    struct process      *next;          // Process table linked list
} process_t;
```

---

## 6. Scheduler

### `run_queue_t` (per-priority-level)
```c
typedef struct {
    thread_t    *head;
    thread_t    *tail;
    uint32_t    count;
} run_queue_t;

// Global scheduler state
typedef struct {
    run_queue_t queues[256];        // One queue per priority level
    thread_t    *current;           // Currently running thread
    thread_t    *idle_thread;       // Fallback when all queues empty
    uint64_t    tick_count;         // Monotonic tick counter
    spinlock_t  lock;
} scheduler_t;
```

### `wait_queue_t`
```c
typedef struct wait_queue {
    thread_t    *head;
    spinlock_t  lock;
} wait_queue_t;
```

---

## 7. Synchronisation Primitives

### `mutex_t`
```c
typedef struct {
    thread_t        *owner;         // NULL = unlocked
    int             owner_orig_prio; // Saved priority before inheritance boost
    wait_queue_t    waiters;
    spinlock_t      lock;
} mutex_t;
```

### `condvar_t`
```c
typedef struct {
    wait_queue_t    waiters;
} condvar_t;
```

### `spinlock_t`
```c
typedef struct {
    volatile int    locked;         // 0 = free; 1 = held
    // Acquired with LOCK XCHG; released with MOV [lock], 0
} spinlock_t;
```

---

## 8. Work Queue and Deferred Tasks

### `work_item_t`
```c
typedef struct work_item {
    void                (*fn)(void *arg);
    void                *arg;
    struct work_item    *next;
} work_item_t;
```

### `work_queue_t`
```c
typedef struct {
    work_item_t *head;
    work_item_t *tail;
    spinlock_t  lock;
    condvar_t   has_work;
} work_queue_t;
// Global: system_wq
```

### `deferred_task_t`
```c
typedef struct {
    uint64_t        fire_at_ns;     // Monotonic ns; fire once
    void            (*fn)(void *);
    void            *arg;
} deferred_task_t;
```

---

## 9. Block Device and Sector Cache

### `block_device_t`
```c
typedef struct {
    uint32_t    dev_id;
    uint64_t    sector_count;
    uint32_t    sector_size;        // Always 512
    int         (*read)(uint32_t dev_id, uint64_t lba, void *buf);
    int         (*write)(uint32_t dev_id, uint64_t lba, const void *buf);
    wait_queue_t irq_queue;         // ATA IRQ wakeup
} block_device_t;
```
Drivers: ATA PIO (IRQ-driven, LBA48), AHCI DMA (PCI class 0x01/0x06), NVMe (PCI class 0x01/0x08)

### `cache_entry_t` (LRU sector cache, 64 entries)
```c
typedef struct {
    uint32_t    dev_id;
    uint64_t    lba;
    uint8_t     data[512];
    bool        dirty;
    uint64_t    last_access;        // Monotonic tick; used for LRU eviction
    bool        valid;
} cache_entry_t;

// Global array: cache_entry_t sector_cache[64];
// Spinlock: sector_cache_lock;
```

---

## 10. Journaling / WAL

### `wal_entry_t`
```c
typedef struct {
    uint8_t     type;               // WAL_DATA (1) or WAL_COMMIT (2)
    uint64_t    lba;                // Target sector on block device
    uint8_t     data[512];          // Sector data (for DATA entries; unused for COMMIT)
    uint32_t    txn_id;             // Transaction ID; DATA and COMMIT share same txn_id
    uint32_t    checksum;           // CRC32 of data field
} wal_entry_t;
```

### `wal_state_t`
```c
typedef struct {
    wal_entry_t ring[63];           // Circular buffer of 63 slots
    uint32_t    head;               // Next write slot (% 63)
    uint32_t    tail;               // Oldest committed slot
    uint32_t    next_txn_id;
    spinlock_t  lock;
} wal_state_t;
```

**Recovery procedure at mount:**
1. Scan all 63 slots
2. Group DATA entries by txn_id
3. If txn_id has matching COMMIT entry: replay all DATA entries (write to disk)
4. If no COMMIT: discard DATA entries (partial transaction)

---

## 11. SFS On-Disk Layout

### Superblock (sector 0)
```c
typedef struct {
    uint32_t    magic;              // 0x5346530A ("SFS\n")
    uint32_t    version;
    uint64_t    total_blocks;
    uint64_t    free_blocks;
    uint64_t    inode_count;
    uint64_t    free_inodes;
    uint32_t    block_size;         // Always 512 (1 sector per block currently)
    uint32_t    inode_table_start;  // Sector number
    uint32_t    inode_bitmap_start;
    uint32_t    block_bitmap_start;
    uint32_t    data_start;
    uint32_t    journal_start;      // First sector of 63-slot WAL ring
    uint8_t     _pad[428];          // Pad to 512 bytes
} sfs_superblock_t;
```

### Inode (on-disk, 128 bytes)
```c
typedef struct {
    uint32_t    mode;               // Permission bits + type (S_IFREG, S_IFDIR, S_IFLNK)
    uint32_t    uid;                // Owner (unused until Stage 6)
    uint32_t    gid;
    uint32_t    nlink;              // Hard link count
    uint64_t    size;               // File size in bytes
    uint64_t    atime, mtime, ctime;
    uint32_t    direct[12];         // Direct block pointers
    uint32_t    indirect1;          // Singly-indirect block pointer
    uint32_t    indirect2;          // Doubly-indirect block pointer
    uint32_t    flags;              // SFS_FLAG_LOCKED (advisory lock)
    uint8_t     _pad[8];            // Pad to 128 bytes
} sfs_inode_t;
```
- Max file size: 12 + 128 + 128² = 16,524 blocks = ~8 MB at 512 B/block

### Directory Entry (on-disk, 32 bytes)
```c
typedef struct {
    uint32_t    inode_id;           // 0 = deleted (slot available for reuse)
    uint8_t     name_len;
    char        name[27];           // Max 26 chars + null terminator
} sfs_dirent_t;
```

---

## 12. VFS In-Memory Structures

### `vnode_t` (in-memory inode cache entry)
```c
typedef struct vnode {
    uint64_t    inode_id;
    vfs_t       *fs;                // Owning filesystem (dispatch table)
    uint32_t    mode;
    uint32_t    uid, gid;
    uint64_t    size;
    uint64_t    atime, mtime, ctime;
    uint32_t    nlink;
    uint32_t    refcount;           // In-memory reference count
    int         lock_flag;
    struct vnode *next;             // Hash chain
} vnode_t;
```

### `mount_t`
```c
typedef struct {
    char        prefix[64];         // Mount point path (e.g. "/tmp")
    vfs_t       *fs;
    bool        read_only;
} mount_t;

// Global: mount_t mount_table[8];
```

### `vfs_t` (filesystem dispatch table)
```c
typedef struct vfs {
    int (*open)(vfs_t*, const char *path, int flags, vnode_t **out);
    int (*read)(vnode_t*, void *buf, size_t n, uint64_t offset);
    int (*write)(vnode_t*, const void *buf, size_t n, uint64_t offset);
    int (*create)(vfs_t*, const char *path, uint32_t mode);
    int (*unlink)(vfs_t*, const char *path);
    int (*mkdir)(vfs_t*, const char *path, uint32_t mode);
    int (*rename)(vfs_t*, const char *old, const char *newpath);
    int (*readdir)(vnode_t*, uint32_t index, sfs_dirent_t *out);
    int (*stat)(vnode_t*, stat_t *out);
    int (*chmod)(vnode_t*, uint32_t mode);
    int (*ftruncate)(vnode_t*, uint64_t size);
    int (*ioctl)(vnode_t*, uint64_t cmd, void *arg);
} vfs_t;
```

### Socket FD Dispatch
```c
// In sys_read/sys_write/sys_close/sys_ioctl:
// If fd is a socket (sock_lookup succeeds), dispatch to sock_ops->recv/send/close/ioctl
// Otherwise, dispatch to vfs_read/vfs_write/vfs_close/vfs_ioctl
socket_t* sock_lookup(int fd);
```

---

## 13. TTY / Terminal

### `tty_t`
```c
typedef struct {
    uint8_t     raw_buf[TTY_BUF_SIZE];     // ISR-fed raw byte ring (4096 bytes)
    uint32_t    raw_head, raw_tail;
    uint8_t     canon_line[1024];           // Canonical mode line buffer
    uint32_t    canon_len;
    termios_t   termios;                    // c_lflag with ICANON, ECHO, ISIG, TOSTOP
    pid_t       fg_pgid;                    // Foreground process group for signal delivery
    wait_queue_t read_queue;                // Threads blocked on tty_read()
    work_item_t sig_work;                   // Work item for ISR-level signal delivery
    spinlock_t  lock;
    bool        sig_pending;                // Signal pending flag
    vfs_node_t  *vnode;                     // VFS node for this TTY
    int         refcount;
} tty_t;
```

### `termios_t`
```c
typedef struct {
    uint32_t    c_iflag;    // Input flags (IGNBRK, BRKINT, etc.)
    uint32_t    c_oflag;    // Output flags (OPOST, ONLCR, etc.)
    uint32_t    c_cflag;    // Control flags (CSIZE, PARENB, etc.)
    uint32_t    c_lflag;    // Local flags: ICANON, ECHO, ECHOE, ISIG, TOSTOP
    uint8_t     c_cc[TTY_CC_NCCS];  // Control characters: VINTR, VQUIT, VERASE, VKILL, VEOF, etc.
} termios_t;
```

**Control character defaults:** VINTR=Ctrl-C(3), VQUIT=Ctrl-\(28), VERASE=DEL(127), VKILL=Ctrl-U(21), VEOF=Ctrl-D(4), VWERASE=Ctrl-W(23)

**PTY (pseudo-terminal):**
```c
// 8-slot PTY pool in pty.c
typedef struct {
    tty_t       *slave;     // Slave TTY (standard line discipline)
    int         master_fd;  // Master VFS fd (raw input/output)
    int         slave_fd;   // Slave VFS fd (standard TTY behaviour)
    bool        used;
} pty_pair_t;
```

---

## 14. Network Structures (Stage 5 — Implemented)

### 14.1 Socket Layer

```c
// sock_ops_t: dispatch table per socket type
typedef struct sock_ops {
    int (*bind)(void *proto, const void *addr, socklen_t addrlen, socket_t *s);
    int (*connect)(void *proto, const void *addr, socklen_t addrlen);
    int (*listen)(void *proto, int backlog);
    int (*accept)(void *proto, void *addr, socklen_t *addrlen, socket_t *newsock, void **newproto);
    int (*send)(void *proto, const void *buf, size_t len, int flags);
    int (*recv)(void *proto, void *buf, size_t len, int flags);
    int (*close)(void *proto);
    int (*poll)(void *proto, int events);
    int (*getsockname)(void *proto, void *addr, socklen_t *addrlen, socket_t *s);
    int (*getpeername)(void *proto, void *addr, socklen_t *addrlen);
    int (*setsockopt)(void *proto, int level, int optname, const void *optval, socklen_t optlen);
    int (*getsockopt)(void *proto, int level, int optname, void *optval, socklen_t *optlen);
} sock_ops_t;

// Global ops tables: tcp_ops, udp_ops
```

```c
typedef struct socket {
    int             domain;         // AF_INET or AF_INET6
    int             type;           // SOCK_STREAM or SOCK_DGRAM
    int             protocol;
    sock_ops_t      *ops;           // tcp_ops or udp_ops
    void            *proto;         // tcp_conn_t* or udp_endpoint_t*
    int             refcount;
    int             fd;             // FD slot index
    bool            ipv6only;       // IPV6_V6ONLY flag
    uint32_t        recv_timeout;   // SO_RCVTIMEO (ms)
    uint32_t        send_timeout;   // SO_SNDTIMEO (ms)
    bool            used;
} socket_t;

// Socket table: socket_t sockets[SOCKET_MAX] (32 entries)
// FD table: sockets occupy FD slots 3+ (0/1/2 = TTY)
```

### 14.2 TCP Connection

```c
typedef struct tcp_conn {
    uint8_t used;
    uint8_t closed;
    uint8_t syn_recv_deferred;      // Data arrived before on_connect set on_recv
    uint8_t af;                     // AF_INET or AF_INET6

    // 5-tuple
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t src_ip6[16], dst_ip6[16];

    // TCP state
    uint8_t state;                  // TCP_CLOSED(0) through TCP_TIME_WAIT(9)

    // Sequence numbers
    uint32_t snd_nxt, snd_una, iss;
    uint32_t rcv_nxt, irs;
    uint16_t snd_wnd, rcv_wnd;

    // Retransmit timers (ms)
    uint32_t rto_remaining;
    uint32_t retry_count;
    uint32_t fin_rto_remaining;
    uint32_t fin_retry_count;
    uint32_t syn_retry_count;

    // Retransmit buffer (last MSS segment)
    uint8_t *retransmit_buf;
    uint16_t retransmit_len;
    uint32_t retransmit_seq;

    // TIME_WAIT
    uint32_t timewait_ms;           // Countdown from 60000 (60s 2MSL)

    // Receive buffer
    uint8_t recv_buf[TCP_MSS];      // Payload copied from eth_rx_poll stack buffer
    uint16_t recv_len;
    bool recv_done;

    // Callbacks
    void (*on_connect)(struct tcp_conn *c);
    void (*on_recv)(struct tcp_conn *c, const uint8_t *data, uint16_t len);
    void (*on_close)(struct tcp_conn *c);

    // Options
    bool nodelay;                   // TCP_NODELAY

    // Socket binding
    socket_t *sock;
} tcp_conn_t;

// Connection table: tcp_conn_t tcp_conns[TCP_MAX_CONNS] (16 entries)
// TCP_MSS = 1460 (1500 ethernet MTU - 20 IPv4 - 20 TCP)
```

### 14.3 UDP Endpoint

```c
#define UDP_DGRAM_QUEUE 16
#define UDP_DGRAM_SIZE 1500

typedef struct {
    uint8_t  payload[UDP_DGRAM_QUEUE][UDP_DGRAM_SIZE];
    uint16_t payload_len[UDP_DGRAM_QUEUE];
    uint32_t src_addr[UDP_DGRAM_QUEUE];        // IPv4 source
    uint16_t src_port[UDP_DGRAM_QUEUE];
    uint8_t  src_addr6[UDP_DGRAM_QUEUE][16];   // IPv6 source
    int      af[UDP_DGRAM_QUEUE];

    volatile int q_count;       // Number of datagrams in queue
    volatile int q_head;        // Dequeue index
    volatile int q_tail;        // Enqueue index

    spinlock_t   lock;
    wait_queue_t recv_wait;

    uint32_t recv_timeout;      // ms
    bool     bound;
    uint8_t  ipv6only;

    // Bind parameters (stored for matching)
    int      bind_af;
    uint32_t bind_addr;
    uint8_t  bind_addr6[16];
    uint16_t bind_port;
} udp_endpoint_t;

// Endpoint table: udp_endpoint_t udp_endpoints[UDP_MAX_ENDPOINTS] (8 entries)
```

### 14.4 Ethernet / ARP / NDP

```c
// EtherType dispatch
typedef void (*eth_handler_t)(const uint8_t *frame, uint16_t len, uint16_t ethertype);
// Registered handlers: ETHERTYPE_IPV4 (0x0800), ETHERTYPE_ARP (0x0806), ETHERTYPE_IPV6 (0x86DD)

// ARP cache (8 entries)
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
} arp_cache_entry_t;

// NDP cache (8 entries)
typedef struct {
    uint8_t  ip[16];
    uint8_t  mac[6];
    bool     valid;
    uint64_t last_used;
} ndp_cache_entry_t;
```

### 14.5 IGMPv2

```c
#define IGMP_MAX_GROUPS 8

typedef struct {
    uint32_t group_addr;
    bool     active;
} igmp_group_t;
```

### 14.6 IPv6 Multicast Groups

```c
#define IPV6_MCAST_MAX_GROUPS 8

typedef struct {
    uint8_t  group_addr[16];
    bool     active;
} ipv6_mcast_group_t;
```

### 14.7 Routing Table

```c
#define ROUTE_TABLE_SIZE 8

typedef struct {
    uint8_t  af;             // AF_INET or AF_INET6
    uint32_t dest_v4;
    uint32_t mask_v4;
    uint32_t gw_v4;
    uint8_t  dest_v6[16];
    uint8_t  mask_v6[16];
    uint8_t  gw_v6[16];
    int      if_index;
    bool     used;
} route_entry_t;
```

---

## 15. TTY / Terminal (cont.)

Contents: see §13 above. TTY structures are unchanged from v1.0 layout, with added fields `termios`, `sig_pending`, `sig_work`, `refcount`, `vnode`.

---

## 16. Planned Schemas (Not Yet Implemented)

### 16.1 Capability Token (Stage 6)
```c
typedef struct {
    uint64_t    token;              // 64-bit CSPRNG value
    uint32_t    rights;             // CAP_READ(1) | CAP_WRITE(2) | CAP_EXEC(4) | CAP_DELEGATE(8)
    uint32_t    obj_type;           // CAPOBJ_FILE | CAPOBJ_PROCESS | CAPOBJ_DEVICE | CAPOBJ_IPC
    uint64_t    obj_id;             // Identifies the object within its type
    uint64_t    parent_token;       // 0 for root caps; set for derived caps
    bool        revoked;
} capability_t;
```

### 16.2 IPC Message Queue (Stage 7)
```c
typedef struct {
    uint8_t     buf[4096];
    uint32_t    head, tail, count;
    uint32_t    max_msg_size;       // Max bytes per message
    wait_queue_t senders;
    wait_queue_t receivers;
    spinlock_t  lock;
    uint32_t    msgs_this_100ms;    // Rate limiter
    uint64_t    rate_window_start;
} ipc_queue_t;
```

### 16.3 Service Descriptor (Stage 7)
```c
typedef struct {
    char        name[32];
    char        binary[128];
    char        *argv[16];
    char        *deps[8];
    enum {
        RESTART_NEVER,
        RESTART_ON_FAILURE,
        RESTART_ALWAYS
    } restart_policy;
    pid_t       pid;                // 0 = not running
    uint64_t    last_ping_ns;
    uint32_t    missed_pings;
    bool        running;
} service_t;
```

### 16.4 procfs Virtual Files (Stage 7)
```
/proc/<pid>/status  — generated on read from process_t fields
/proc/<pid>/maps    — generated on read by walking process PML4
/proc/<pid>/fd/     — symlinks generated from fd_table entries
/proc/meminfo       — generated from pmm_state_t + slab stats
/proc/uptime        — generated from monotonic tick counter
```
No on-disk backing; all content generated at read time from live kernel data structures.
