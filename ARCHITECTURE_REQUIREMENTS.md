# LAYERED OPERATING SYSTEM ARCHITECTURE
## Complete Design & Requirements Document

---

# 1. ARCHITECTURE OVERVIEW

## 1.1 Design Philosophy

This OS follows a **strict layered architecture** where each layer N:
- **Communicates only with layer N-1 below** via a defined downward API
- **Never calls upward** — the only exception is registered callback hooks (event bus subscriptions)
- **Never skips layers** — Layer 5 cannot invoke Layer 1 directly
- **Is a replaceable module** — each layer has a versioned API, can be unit-tested independently
- **Has debug mode** — trace logging, health checks, fault injection

## 1.2 Annotated Stack Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│  LAYER N: USER PROGRAMS                                                  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                    │
│  │  init     │ │  shell   │ │  daemons │ │  user    │   ...              │
│  │          │ │          │ │          │ │  apps   │                     │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘                    │
│       │            │            │            │                            │
│       └────────────┴────────────┴────────────┘                            │
│                    │  libc / syscall stubs                                │
├────────────────────┼─────────────────────────────────────────────────────┤
│  LAYER N-1: SYSCALL GATEWAY + SHELL                                      │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  Syscall Dispatcher   │  Audit Logger   │  Rate Limiter           │  │
│  │  Circuit Breaker      │  Capability Verifier                      │  │
│  └────────────────────────────────────────────────────────────────────┘  │
├══════════════════════════════════════════════════════════════════════════╡
│                    ── KERNEL BOUNDARY (Privilege Ring 0) ──              │
├══════════════════════════════════════════════════════════════════════════╡
│  LAYER 5: I/O, FILE SYSTEM & DEVICE MANAGEMENT                           │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  VFS        │  FS Drivers   │  Block Cache   │  Device Manager   │  │
│  │  (ext2,     │  (ext2, fat,  │  (page cache)  │  (dev fs,        │  │
│  │   tmpfs)   │   devfs)      │                │   device mapper)  │  │
│  └────────────────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────────────┤
│  LAYER 4: VIRTUAL MEMORY, PAGING & MEMORY PROTECTION                     │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  Page Alloc │  VM Mapper  │  Swap Mgr    │  MP Enforcer  │  OOM   │  │
│  │  (buddy)   │  (page tbl) │  (eviction)  │  (COW, guard)│  Killer│  │
│  └────────────────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────────────┤
│  LAYER 3: PROCESS LIFECYCLE, THREADS & SYNCHRONIZATION                   │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  Process Mgr│  Thread Mgr  │  Sync Prims   │  Capability Mgr     │  │
│  │  (fork/    │  (spawn/     │  (mutex,      │  (token issue/      │  │
│  │   exec/exit)│   join)     │   condvar,    │   revoke)           │  │
│  │            │             │   rwlock)     │                     │  │
│  └────────────────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────────────┤
│  LAYER 2: CPU SCHEDULING, IPC & INTERRUPT HANDLING                       │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  Scheduler  │  IPC (msg   │  Interrupt    │  Timekeeping          │  │
│  │  (round     │   queues,   │  Dispatcher   │  (monotonic clock,   │  │
│  │   robin/PRI)│   shmem)    │  (IDT, ISR)   │   timer namespaces)  │  │
│  └────────────────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────────────┤
│  LAYER 1: HARDWARE ABSTRACTION LAYER (HAL) + DRIVER MODEL                │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  CPU HAL    │  Timer HAL  │  UART HAL    │  Block HAL   │ PIC HAL │  │
│  │  Driver Mgr │  DMA Mgr    │  MMIO Mapper  │  Interrupt Ctrl       │  │
│  └────────────────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────────────┤
│  LAYER 0: HARDWARE                                                        │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐ │
│  │  CPU     │ │  RAM     │ │  UART    │ │  Block   │ │ Timer + PIC  │ │
│  │  (x86)  │ │ (512MB)  │ │ (serial) │ │ (storage)│ │ (IRQ0, etc)  │ │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────────┘ │
└──────────────────────────────────────────────────────────────────────────┘

## 1.3 Cross-Cutting Concerns (Horizontal)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  WATCHDOG DAEMON (out-of-band, polls all layers)                        │
├──────────────────────────────────────────────────────────────────────────┤
│  UNIFIED EVENT BUS (pub/sub across layers 1-5)                          │
├──────────────────────────────────────────────────────────────────────────┤
│  WRITE-AHEAD LOG (WAL) — kernel state journaling                        │
├──────────────────────────────────────────────────────────────────────────┤
│  CAPABILITY-BASED SECURITY MODEL                                        │
├──────────────────────────────────────────────────────────────────────────┤
│  GRACEFUL DEGRADATION (NORMAL → SAFE → EMERGENCY)                      │
└──────────────────────────────────────────────────────────────────────────┘
```

## 1.4 Layer Communication Rules

| Rule | Description | Violation Consequence |
|------|-------------|----------------------|
| Strict layering | L_N calls only L_{N-1} | Compile-time error (module boundary) |
| No upward calls | L_N never invokes L_{N+1} | Circular dependency detector panics |
| Event bus only | Cross-cutting events via pub/sub | Event is dropped, logged as security event |
| Capability gate | Every cross-layer op requires token | Access denied, audit logged |
| No layer skipping | L_5 → L_1 disallowed | Kernel panic / forbidden call trap |

## 1.5 Operating Modes

| Mode | Active Layers | Permitted Operations | Restrictions |
|------|---------------|---------------------|--------------|
| NORMAL | 0–5, N-1, N | All | None |
| SAFE | 0–3, N-1 (limited) | Essential processes only, no fork/exec | Layer 4,5 restricted; no new process creation; filesystem read-mostly |
| EMERGENCY | 0–2 | RAM-only, FS sealed read-only, no user processes | Only watchdog + recovery threads; panic on any write to storage |

Mode transitions are initiated by the Watchdog Daemon or Layer 3 when resource exhaustion is detected.

---

# 2. LAYER 0: HARDWARE

## 2A. Layer Specification

**Purpose**: Represent the physical machine. This layer is not software — it is the target hardware that all other layers abstract.

**Responsibilities**:
- Provide CPU with protected modes (real mode → protected mode → long mode for x86-64)
- Provide RAM with physical addressing (no caching, no translation)
- Generate timer interrupts (IRQ0) at configurable frequency
- Provide UART serial I/O for debug console
- Provide block storage with LBA (Logical Block Addressing)
- Provide Programmable Interrupt Controller (PIC/APIC) for interrupt routing

**Does NOT own**:
- Memory translation (that's Layer 4)
- Scheduling (Layer 2)
- Device drivers (Layer 1)

## 2B. Internal Module Breakdown

N/A — this is hardware. But the architecture assumes:

| Component | Specification | Notes |
|-----------|--------------|-------|
| CPU | x86-64, 1 core (scalable to N) | Supports rings 0/3, paging, MSRs |
| RAM | 512 MB minimum, byte-addressable | Physically contiguous, no MMU bypass |
| UART | 16550-compatible serial port | MMIO at fixed address 0x3F8 (COM1) |
| Block Device | ATA/IDE or NVMe, 512B sectors | LBA48 addressing |
| Timer | PIT (IRQ0) or HPET | Programmable interval |
| PIC | 8259A or xAPIC | IRQ routing, masking |

## 2C. Data Structures

None (hardware-defined registers).

## 2D. API Surface (Downward Interface)

None — this is the bottom layer.

## 2E. Interaction Contract with Layer Below

N/A — bottom of stack.

## 2F. Edge Cases and Fault Handling

| Condition | Behavior |
|-----------|----------|
| CPU triple-fault | Hardware resets; WAL replay on next boot |
| RAM ECC error (if present) | Machine check exception → Layer 2 handles via MCE handler |
| Disk sector unreadable | ATA error register set → Layer 1 HAL returns ERR_IO |
| Spurious interrupt | PIC returns vector 0 → Layer 2 ignores, logs |
| Watchdog timeout (hardware) | NMI → unconditional panic, dump registers |

---

# 3. LAYER 1: HARDWARE ABSTRACTION LAYER + DRIVER MODEL

## 3A. Layer Specification

**Purpose**: Abstract all hardware specifics behind a uniform, portable API. Drivers plug into this layer. Layer 2 onward never touch hardware registers directly.

**Responsibilities**:
- CPU mode initialization (enter protected/long mode)
- Interrupt descriptor table (IDT) setup
- Timer programming (PIT/HPET)
- UART read/write with buffering
- Block device read/write with DMA
- PIC/APIC configuration and IRQ management
- MMIO region mapping for device access
- Driver registration, enumeration, and hot-plug

**Does NOT own**:
- Interrupt handling logic (Layer 2 decodes and dispatches)
- Process scheduling (Layer 2)
- Memory paging (Layer 4 — HAL manages only physical pages via a simple frame allocator)

## 3B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `cpu_hal` | Writes MSRs, sets up GDT, writes IDT entries, manages control registers (CR0, CR3, CR4) |
| `timer_hal` | Programs PIT/HPET frequency, provides `sleep_usec` busy-wait |
| `uart_hal` | Configures baud rate, line settings; provides `putchar`/`getchar` with optional IRQ-driven RX |
| `block_hal` | Sends ATA/PCI commands via PIO or DMA; abstracts sector I/O |
| `pic_hal` | Masks/unmasks IRQs, sends EOI, handles IRQ routing |
| `driver_mgr` | Maintains driver registry (linked list of `driver_t`); handles `probe`, `attach`, `detach` lifecycle |
| `dma_mgr` | Manages DMA channel allocation and bus-mastering setup |
| `mmio_mapper` | Maps device MMIO regions via the page tables (simple identity map or assigned window) |
| `physical_frame_allocator` | Bump allocator at boot; bitmap allocator after initialization tracks free physical frames (4KB each) for 512MB RAM = 131072 entries |

## 3C. Data Structures

```c
// Driver descriptor — every driver fills this
struct driver_t {
    char name[64];
    uint32_t version;           // semantic version packed: MAJOR<<16|MINOR<<8|PATCH
    uuid_t driver_id;
    // Lifecycle
    err_t (*probe)(void);       // detect hardware presence
    err_t (*attach)(void);      // initialize device
    void  (*detach)(void);      // shutdown device
    // IRQ handling — registered with Layer 2
    void  (*isr)(irq_num_t irq, void* context);
    void* context;
    // Capabilities
    capability_t required_caps; // driver capabilities needed to attach
};

// Interrupt Descriptor Table
struct idt_entry_t {
    uint16_t offset_low;        // ISR address low 16 bits
    uint16_t segment_selector;  // code segment (kernel CS)
    uint8_t  ist;               // interrupt stack table offset
    uint8_t  type_attr;         // present, DPL, gate type
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

// Physical frame allocator bitmap
struct frame_bitmap_t {
    uint64_t total_frames;      // total physical frames available
    uint64_t free_frames;       // current free count
    uint8_t* bitmap;            // one bit per 4KB frame (0=free, 1=allocated)
    spinlock_t lock;            // SMP: protects bitmap
};

// MMIO region descriptor
struct mmio_region_t {
    uintptr_t phys_base;
    uintptr_t virt_base;
    uint64_t  size_bytes;
    uint32_t  flags;            // CACHED | UNCACHED | WRITE_COMBINE
};
```

## 3D. API Surface (Downward Interface)

Exposed to Layer 2:

```
FUNCTION: hal_init(boot_info_t* info) → err_t
PRECONDITIONS: Called from boot.s after real-mode init. info contains:
               - memory map (e820 entries)
               - framebuffer info
               - RSDP (ACPI root) pointer
POSTCONDITIONS: GDT, IDT, PIC, timer, UART initialized. System in long mode (x86-64).
                Driver manager runs auto-probe on all known buses.
SIDE EFFECTS: Physical frame bitmap initialized. Drivers loaded.

FUNCTION: hal_timer_set_freq(uint32_t hz) → err_t
PRECONDITIONS: hal_init done. hz ∈ [1, 10000].
POSTCONDITIONS: Timer fires at hz.
SIDE EFFECTS: PIT/HPET counter registers reprogrammed.

FUNCTION: hal_uart_write(char c) → err_t
PRECONDITIONS: UART initialized.
POSTCONDITIONS: Character queued (or busy-wait if in synchronous mode).
SIDE EFFECTS: TX buffer shifts; may trigger TX interrupt.

FUNCTION: hal_uart_read(char* out, uint32_t timeout_ms) → err_t
PRECONDITIONS: UART initialized. out != NULL.
POSTCONDITIONS: out contains next character or ERR_TIMEOUT.
SIDE EFFECTS: None (polled) or RX buffer consumed (IRQ mode).

FUNCTION: hal_block_read(uint64_t lba, uint32_t count, void* buffer) → err_t
PRECONDITIONS: count > 0 && count <= 256. buffer page-aligned.
POSTCONDITIONS: buffer filled with sector data or ERR_IO on failure.
SIDE EFFECTS: ATA command issued; may use DMA.

FUNCTION: hal_block_write(uint64_t lba, uint32_t count, const void* buffer) → err_t
PRECONDITIONS: count > 0 && count <= 256. buffer page-aligned.
POSTCONDITIONS: Sectors written to device. Returns ERR_IO on failure.
SIDE EFFECTS: ATA command issued; write cache flushed.

FUNCTION: hal_irq_mask(irq_num_t irq, bool masked) → err_t
PRECONDITIONS: irq ∈ [0, 15] for PIC, [0, 255] for APIC. hal_init done.
POSTCONDITIONS: IRQ enabled/disabled in controller.
SIDE EFFECTS: PIC/APIC mask register updated.

FUNCTION: hal_irq_send_eoi(irq_num_t irq) → void
PRECONDITIONS: Called within ISR context.
POSTCONDITIONS: PIC/APIC acknowledges interrupt completion.
SIDE EFFECTS: Writes to PIC/APIC EOI register.

FUNCTION: hal_phys_alloc_frame(void) → uintptr_t | ERR_OOM
PRECONDITIONS: Frame allocator initialized. IRQs disabled (atomic).
POSTCONDITIONS: Returns 4KB-aligned physical address. Bitmap updated.
SIDE EFFECTS: Frames bitmap bit set to 1.

FUNCTION: hal_phys_free_frame(uintptr_t phys_addr) → err_t
PRECONDITIONS: phys_addr is 4KB-aligned, was previously allocated.
POSTCONDITIONS: Frame returned to free pool.
SIDE EFFECTS: Bitmap bit cleared.

FUNCTION: hal_mmio_map(uintptr_t phys, uint64_t size, uint32_t flags) → uintptr_t | ERR
PRECONDITIONS: phys aligned to page boundary.
POSTCONDITIONS: Returns virtual address where phys is mapped.
SIDE EFFECTS: Page table entry updated.

FUNCTION: hal_driver_register(driver_t* drv) → err_t
PRECONDITIONS: drv != NULL, all function pointers set.
POSTCONDITIONS: Driver added to registry; probe() called.
SIDE EFFECTS: Driver list updated; hardware may be detected.

FUNCTION: hal_shutdown() → err_t
PRECONDITIONS: All layers called their shutdown.
POSTCONDITIONS: All devices detached; system ready for poweroff (or ACPI shutdown).
SIDE EFFECTS: Disables interrupts; driver detach all.
```

## 3E. Interaction Contract with Layer Below

**Expectations from Layer 0 (Hardware)**:
- CPU starts in real mode; HAL transitions to protected/long mode
- RAM is readable/writable at physical addresses
- Timer generates periodic IRQ0
- UART registers accessible via MMIO or port I/O
- Block device responds to ATA/PCI commands
- PIC delivers wired interrupts

**Guarantees to Layer 1's callers (Layer 2)**:
- All hardware details abstracted — no port I/O or raw MMIO required above
- Atomicity: Critical operations (frame alloc, IRQ mask) disable interrupts on uniprocessor; spinlock on SMP
- Error propagation: Every function returns an `err_t`; no silent failures
- Interrupt gate: ISR entry/exit is handled; callers provide only the handler
- Driver lifecycle: Safe attach/detach; a driver failure does not bring down the HAL

## 3F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **Driver probe fails** | Log via UART; mark driver slot as INACTIVE; continue boot; no panic |
| **Driver probe hangs** | Watchdog at Layer 2 detects timeout (5s); watchdog NMI fires; driver is force-detached |
| **Physical memory exhausted (OOM in bitmap)** | `hal_phys_alloc_frame` returns ERR_OOM; caller must handle |
| **Block I/O timeout** | HAL retries once; if second attempt also fails, returns ERR_IO; no retry internally |
| **UART buffer overflow** | Oldest character dropped; `hal_uart_stats.overrun` incremented |
| **SMP spinlock contention** | Minimal spinning (100 spins) then pause instruction; lock holder preemption handled via priority inheritance |
| **Hot-plug device detection** | Only if PCIe native hot-plug present; driver_mgr enumerates on bus rescan |
| **DMA buffer not aligned** | Returns ERR_INVAL; caller must provide physically-contiguous page-aligned buffer |
| **Power loss mid-write** | No recovery at HAL level; Layer 5 (FS) with WAL ensures consistency |
| **Double free of physical frame** | Frame bitmap integrity check catches (free of already-free frame) → returns ERR_DOUBLE_FREE |

## 3G. Debuggability Interface

```c
// Layer 1 debug APIs — called by Watchdog or debug shell

err_t hal_trace(trace_verbosity_t v);
    // Verbosity LOW:   log every driver attach/detach, every alloc/free frame
    // Verbosity MED:   add all I/O operations (block r/w, UART r/w, IRQ mask)
    // Verbosity HIGH:  add every register read/write, every spinlock acquire/release
    // Output: JSON-encoded structured log via dedicated serial channel

typedef enum {
    HAL_OK = 0,
    HAL_DEGRADED,       // e.g., timer running but UART dead
    HAL_FAILED           // catastrophic (timer stopped, memory bitmap corrupt)
} hal_health_t;

hal_health_t hal_health_check(char* reason_buf, size_t buf_len);
    // Checks:
    //   - Timer: was IRQ0 seen in last 1s? (incrementing counter)
    //   - UART: loopback test passes?
    //   - Block: identify command succeeds?
    //   - Frame bitmap: free+alloc counts match total frames?
    //   - Spinlocks: no deadlock detected?
    // Returns status with human-readable reason string.

typedef enum {
    FAULT_OOM,              // hal_phys_alloc_frame always returns ERR_OOM
    FAULT_IO_TIMEOUT,       // block I/O always times out
    FAULT_UART_SILENT,      // UART write succeeds but char never sent
    FAULT_TIMER_STOP,       // Timer IRQ stops firing
    FAULT_SPINLOCK_DEADLOCK // Inject spinlock that never releases
} hal_fault_type_t;

err_t hal_fault_inject(hal_fault_type_t t);
    // Installs a fault override for testing.
    // Call hal_fault_clear() to restore normal operation.

typedef struct {
    uint64_t frames_allocated;
    uint64_t frames_freed;
    uint64_t block_reads;
    uint64_t block_writes;
    uint64_t block_errors;
    uint64_t uart_tx_chars;
    uint64_t uart_rx_chars;
    uint64_t uart_overruns;
    uint64_t irqs_handled;
    uint64_t timer_ticks;
    float    timer_freq_hz_actual;   // measured vs requested
    uint64_t spinlock_contentions;
} hal_stats_t;

hal_stats_t hal_stats(void);
    // Returns snapshot of all counters. Counters wrap on overflow (uint64).
```

---

# 4. LAYER 2: CPU SCHEDULING, IPC & INTERRUPT HANDLING

## 4A. Layer Specification

**Purpose**: Manage the CPU's execution of threads, route interrupts to registered handlers, and provide inter-process communication primitives. This layer owns the concept of "running" vs "runnable."

**Responsibilities**:
- Thread scheduling (priority-based round robin, O(1) run queues)
- Context switching (save/restore registers and page table pointer)
- Interrupt dispatch: route IRQ → registered handler from Layer 1 or Layer 3 drivers
- Inter-process communication: message queues, shared memory regions
- Timekeeping: monotonic clock, wall clock, timer namespaces, sleep/wake
- Watchdog timer (soft) — detect hung Layers 3–5
- NMI handling for hardware faults

**Does NOT own**:
- Process lifecycle (fork/exec/exit — Layer 3 owns this)
- Thread creation (Layer 3 owns thread_t; Layer 2 only schedules them)
- Synchronization primitives (mutex/condvar — Layer 3 owns them but uses Layer 2 atomic ops)
- Memory management (Layer 4)

## 4B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `scheduler` | Maintains per-priority run queues; selects next thread via O(1) algorithm; handles preemption (timer tick) and yield; manages idle thread |
| `context_switcher` | Saves/restores callee-saved registers, RSP, CR3 (page table base); updates `current_thread` pointer; handles IRET for return-to-user |
| `interrupt_dispatcher` | Maintains IRQ → handler mapping (registered by Layer 1 drivers & Layer 3); invokes ISRs; handles spurious IRQs; delegates IPIs on SMP |
| `ipc` | Message queue primitive (kernel-owned buffer, copy-based or shared memory); implements `send`/`recv` with optional timeout; rendezvous for synchronous IPC |
| `timekeeping` | Tracks monotonic nanosecond counter (from timer IRQ); tracks wall clock (RTC-derived); maintains per-process timer namespaces; handles `sleep`/`nanosleep`; provides `clock_gettime` |
| `ipi_manager` (SMP) | Sends and receives inter-processor interrupts (TLB shootdown, reschedule-IPI, function-call IPI) |

## 4C. Data Structures

```c
// Thread control block (core scheduling unit)
typedef struct thread_t {
    uint64_t id;                    // unique thread ID
    // Context
    uint64_t rsp;                   // kernel stack pointer
    uint64_t cr3;                   // page table base (per-thread or per-process)
    struct thread_context* ctx;     // saved callee-saved regs
    // Scheduling
    enum { READY, RUNNING, BLOCKED, SLEEPING, ZOMBIE } state;
    uint32_t priority;              // 0 (idle) .. 255 (realtime)
    uint64_t time_slice_remaining;  // ticks left before preemption
    uint64_t total_cpu_ticks;       // accumulated CPU time
    uint64_t last_scheduled;        // monotonic timestamp
    // Run queue linkage
    struct thread_t* next;
    struct thread_t* prev;
    // IPC
    struct ipc_queue_t* recv_queue; // messages directed to this thread
    // Timer namespace offset
    int64_t time_offset_ns;         // offset from system monotonic
    // Capabilities
    capability_set_t caps;          // tokens this thread's process holds
    // Debug
    char name[64];
} thread_t;

// Run queue — one per priority level
typedef struct run_queue_t {
    thread_t* head;
    thread_t* tail;
    uint32_t  count;
    spinlock_t lock;
} run_queue_t;

// Priority bitmap — O(1) scheduler
typedef struct {
    run_queue_t queues[256];        // one per priority level
    uint64_t    non_empty_bitmap[4]; // 256 bits, bit N set if queue[N] non-empty
    uint32_t    total_runnable;
    spinlock_t  lock;
} scheduler_t;

// IRQ handler mapping
typedef struct irq_handler_t {
    irq_num_t             irq;
    err_t (*handler)(irq_num_t irq, struct irq_context* ctx, void* data);
    void*                 data;
    struct irq_handler_t* next;     // chaining for shared IRQs
} irq_handler_t;

// IPC message
typedef struct ipc_message_t {
    uint64_t sender_id;
    uint64_t msg_type;              // user-defined
    uint64_t payload_size;          // max 4096 bytes
    uint8_t  payload[0];            // flexible array member
    cap_token_t sender_cap;         // capability token for validation
} ipc_message_t;

// IPC queue (kernel-resident, bounded)
typedef struct ipc_queue_t {
    ipc_message_t** slots;          // ring buffer
    uint32_t        capacity;       // power of two
    uint32_t        head;
    uint32_t        tail;
    uint32_t        count;
    uint32_t        max_msg_size;
    spinlock_t      lock;
    condvar_t       not_empty;      // for blocking recv
    condvar_t       not_full;       // for blocking send
} ipc_queue_t;

// Time namespace
typedef struct time_ns_t {
    uint64_t monotonic_offset_ns;   // added to hardware counter
    uint64_t wall_clock_offset_ns;  // offset from epoch
    bool     is_isolated;           // if true, wall clock doesn't advance
} time_ns_t;

// SMP: per-CPU data
typedef struct percpu_t {
    uint32_t        cpu_id;
    thread_t*       current_thread;
    thread_t*       idle_thread;
    scheduler_t*    scheduler;
    uint64_t        tsc_freq_khz;
    uint64_t        irq_nest_level;    // for irq context detection
    struct thread_context* idle_context;
} percpu_t;
```

## 4D. API Surface (Downward Interface)

Exposed to Layer 3:

```
FUNCTION: sched_yield(void) → void
PRECONDITIONS: Called from thread context.
POSTCONDITIONS: Current thread may be preempted; next runnable thread runs.
SIDE EFFECTS: Context switch.

FUNCTION: sched_set_priority(thread_t* t, uint32_t priority) → err_t
PRECONDITIONS: t != NULL. priority ∈ [0, 255].
POSTCONDITIONS: Thread moved to new priority queue.
SIDE EFFECTS: Run queue modified.

FUNCTION: sched_block(thread_t* t, wait_queue_t* wq) → err_t
PRECONDITIONS: t == current_thread. wq initialized.
POSTCONDITIONS: Thread removed from run queue, added to wait queue. Context switch.
SIDE EFFECTS: Run queue, wait queue updated.

FUNCTION: sched_wake(thread_t* t) → err_t
PRECONDITIONS: t->state == BLOCKED.
POSTCONDITIONS: Thread moved from wait queue to run queue.
SIDE EFFECTS: Run queue updated.

FUNCTION: sched_add_thread(thread_t* t) → err_t
PRECONDITIONS: t initialized with priority and state=READY.
POSTCONDITIONS: Thread added to appropriate run queue.
SIDE EFFECTS: Run queue count ++.

FUNCTION: sched_remove_thread(thread_t* t) → err_t
PRECONDITIONS: t in run queue (state == READY or RUNNING).
POSTCONDITIONS: Thread removed from run queue.
SIDE EFFECTS: Run queue count --.

FUNCTION: sched_get_current_thread(void) → thread_t*
POSTCONDITIONS: Returns current thread TCB. Never returns NULL (idle thread exists).
SIDE EFFECTS: None.

FUNCTION: intr_register_handler(irq_num_t irq, irq_handler_t handler) → err_t
PRECONDITIONS: irq is a valid hardware IRQ line. handler != NULL.
POSTCONDITIONS: Handler added to chain for that IRQ.
SIDE EFFECTS: IRQ unmasked in PIC (if first handler registered).

FUNCTION: intr_unregister_handler(irq_num_t irq, irq_handler_t handler) → err_t
PRECONDITIONS: Handler was previously registered.
POSTCONDITIONS: Handler removed from chain. IRQ masked if chain empty.
SIDE EFFECTS: IRQ mask updated.

FUNCTION: intr_disable(void) → uint64_t
POSTCONDITIONS: Interrupts disabled on current CPU. Returns previous EFLAGS.IF.
SIDE EFFECTS: CLI instruction.

FUNCTION: intr_restore(uint64_t flags) → void
PRECONDITIONS: flags was return value of intr_disable.
POSTCONDITIONS: Interrupts restored to previous state.
SIDE EFFECTS: STI or CLI depending on flags.

FUNCTION: ipc_queue_create(uint32_t capacity, uint32_t max_msg_size) → ipc_queue_t* | ERR
PRECONDITIONS: capacity is power of 2. capacity >= 2. max_msg_size ≤ 4096.
POSTCONDITIONS: Queue allocated in kernel heap; empty.
SIDE EFFECTS: Memory allocated from kernel heap (Layer 4 via page alloc).

FUNCTION: ipc_queue_destroy(ipc_queue_t* q) → err_t
PRECONDITIONS: q is empty (no pending messages).
POSTCONDITIONS: Queue memory freed.
SIDE EFFECTS: Memory returned.

FUNCTION: ipc_send(ipc_queue_t* q, ipc_message_t* msg, uint64_t timeout_ms) → err_t
PRECONDITIONS: q initialized. msg->payload_size <= q->max_msg_size.
                  capability_t present for sending to this queue.
POSTCONDITIONS: If timeout = 0: returns ERR_AGAIN if queue full.
                If timeout > 0: blocks until slot available or timeout.
SIDE EFFECTS: Message copied to kernel queue.

FUNCTION: ipc_recv(ipc_queue_t* q, ipc_message_t* buf, uint64_t timeout_ms) → err_t
PRECONDITIONS: q initialized. buf has at least q->max_msg_size payload.
POSTCONDITIONS: If timeout = 0: returns ERR_AGAIN if queue empty.
                If timeout > 0: blocks until message or timeout.
SIDE EFFECTS: Message copied to caller's buffer.

FUNCTION: clock_get_monotonic_ns(void) → uint64_t
PRECONDITIONS: None.
POSTCONDITIONS: Returns nanoseconds since boot.
SIDE EFFECTS: None.

FUNCTION: clock_get_wall_ns(void) → uint64_t
PRECONDITIONS: RTC has been read at boot.
POSTCONDITIONS: Returns nanoseconds since Unix epoch.
SIDE EFFECTS: None.

FUNCTION: clock_sleep(uint64_t ns) → err_t
PRECONDITIONS: Called from thread context.
POSTCONDITIONS: Thread blocked for at least ns nanoseconds (or more, depending on timer granularity).
SIDE EFFECTS: Thread removed from run queue; timer_wheel entry added.
```

## 4E. Interaction Contract with Layer Below

**Expectations from Layer 1 (HAL)**:
- `hal_timer_set_freq(hz)` — configures periodic timer interrupt
- `hal_irq_mask/enable` — control IRQ delivery
- `hal_irq_send_eoi` — acknowledge interrupt completion
- `hal_phys_alloc_frame` — allocate physical pages for kernel stacks, IPC buffers
- `hal_mmio_map` — map device memory if needed

**Guarantees to Layer 3**:
- O(1) scheduler: constant-time thread selection (priority bitmap lookup)
- Fair CPU distribution within same priority (round-robin with time slices)
- Deterministic context switch: save/restore all callee-saved registers in < 1μs
- IPC atomicity: `send` + `recv` on same queue are serialized by spinlock; no torn reads
- Interrupt handlers run on kernel stack of current thread; re-entrancy tracked via `irq_nest_level`
- Monotonic clock: always non-decreasing, even across suspend/resume (if hardware supports)

## 4F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **Timer IRQ stops firing** | Watchdog detects monotonic clock stall (no increment for 2s); triggers Layer 2 health check -> tries to reinit timer via `hal_timer_set_freq`; if still dead, enters SAFE mode |
| **Run queue empty** | Idle thread runs; idle thread executes `hlt` instruction (power saving); on SMP, idle thread spins with `pause` |
| **IPC queue full** | `ipc_send` with timeout=0 returns ERR_AGAIN; with timeout>0 blocks sender; with timeout=INF blocks indefinitely |
| **IPC queue empty on recv** | `ipc_recv` behavior mirrors send: non-blocking returns ERR_AGAIN; blocking sleeps |
| **Stuck interrupt handler** | If ISR runs > 100μs, watchdog fires; ISR is preempted (if using interrupt threads); handler is detached if it recurs |
| **Spurious IRQ (no handler)** | EOI sent; counter incremented; no further action |
| **NMI (hardware fault)** | Dump register state to serial; call `wal_flush()`; halt system |
| **Deadlock on run queue lock** | Spinlock with timeout — if lock held > 1ms, panic with debug dump (identify holder thread via stack trace) |
| **SMP: TLB shootdown IPI lost** | Sender waits 100μs for ack; on timeout, sends again; after 3 failures, marks remote CPU as UNRESPONSIVE (SAFE mode escalation) |
| **SMP: load imbalance** | Push migration: idle CPU pulls threads from busy CPU's run queue after 2ms of idling |
| **Sleep/wake race (lost wakeup)** | `sched_block` + `sched_wake` protocol uses wait queue spinlock to prevent: thread adds itself to wait queue, then checks condition, then blocks — all under lock |
| **Thread runs past time slice** | Timer interrupt handler decrements `time_slice_remaining`; on 0, reschedule flag set; context switch at next opportunity (kernel exit or explicit yield) |
| **Context switch during interrupt** | Nested interrupts allowed (kernel re-entrancy) but context switch deferred until outermost interrupt completes |

## 4G. Debuggability Interface

```c
err_t sched_trace(trace_verbosity_t v);
    // LOW:   log context switches (from→to thread IDs, reason)
    // MED:   add IPC send/recv, IRQ dispatch
    // HIGH:  add every scheduler bitmap update, every priority change

typedef enum {
    SCHED_OK = 0,
    SCHED_DEGRADED,       // timer jitter > 10%, idle > 90%
    SCHED_FAILED          // timer stopped, no context switch for 5s
} sched_health_t;

sched_health_t sched_health_check(char* reason, size_t len);
    // Checks:
    //   - Timer monotonic advancing
    //   - Run queue counts match known thread count
    //   - No thread blocked > 30s (potential deadlock)
    //   - Context switch rate > 0 in last 1s (if any runnable threads)
    //   - Spinlocks held > threshold (livelock detection)

err_t sched_fault_inject(sched_fault_type_t t);
    // FAULT_SCHED_STOP:   scheduler stops selecting new threads
    // FAULT_IPC_DROP:     ipc_send succeeds but message not delivered
    // FAULT_TIMER_SKEW:   clock_monotonic jumps backward
    // FAULT_IRQ_SPURIOUS: inject 100 spurious IRQs/sec

typedef struct {
    uint64_t context_switches;
    uint64_t preemptions;
    uint64_t voluntary_yields;
    uint64_t ipc_sends;
    uint64_t ipc_recvs;
    uint64_t ipc_send_blocks;      // blocked due to full queue
    uint64_t ipc_recv_blocks;      // blocked due to empty queue
    uint64_t irqs_received;
    uint64_t irqs_spurious;
    uint64_t ipi_sent;             // SMP
    uint64_t ipi_received;         // SMP
    uint64_t timeslices_expired;
    float    avg_context_switch_us; // measured
    float    cpu_busy_pct;          // over last 1s
} sched_stats_t;

sched_stats_t sched_stats(void);
```

> **SMP Extension Notes**: 
> - Each CPU has its own `percpu_t` and its own run queue (affinity-scheduled)
> - `thread_t` gains `cpu_affinity` bitmap and `last_cpu` field
> - IPI types: RESCHEDULE (ask CPU to re-evaluate run queue), TLB_SHOOTDOWN (flush TLB on all CPUs), FUNCTION_CALL (execute function on remote CPU)
> - New race condition: two CPUs concurrently wake same thread → both try to add to run queue → serialized by queue lock
> - New race condition: TLB shootdown IPI arrives while thread switching CR3 → shootdown deferred until after switch completes

---

# 5. LAYER 3: PROCESS LIFECYCLE, THREADS & SYNCHRONIZATION

## 5A. Layer Specification

**Purpose**: Provide the abstraction of a "process" (address space + thread(s) + capabilities) and the primitives for concurrency control. This is the layer where user execution contexts are born, live, and die.

**Responsibilities**:
- Process creation (`fork`/`spawn`), execution (`exec`), and termination (`exit`/`kill`)
- Thread creation and joining
- Synchronization primitives: mutexes, condition variables, read-write locks, semaphores
- Capability management: issue, validate, revoke, delegate tokens
- Process isolation: each process has a unique address space and capability set
- Credentials / ownership metadata

**Does NOT own**:
- Thread scheduling (Layer 2)
- Memory mapping (Layer 4 — but Layer 3 requests mappings for process creation)
- File operations (Layer 5)
- Interrupt handling (Layer 2)

## 5B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `process_mgr` | Manages process table (array of `process_t`); handles `spawn`, `exec`, `exit`, `wait`, `kill`; assigns PIDs; propagates exit status to parent |
| `thread_mgr` | Creates `thread_t` objects (using Layer 2 thread structure); initializes kernel stack and thread context; handles `thread_create`, `thread_exit`, `thread_join` |
| `sync_prims` | Mutex, condvar, rwlock, semaphore implementations; uses Layer 2 `sched_block`/`sched_wake` for wait queues; implements priority inheritance for mutexes |
| `capability_mgr` | Maintains capability table per process; validates tokens on every kernel operation; handles delegation (safely, with depth limits); revocation marks tokens invalid |
| `process_cred` | Associates UID/GID with process; governs signal delivery permissions; (legacy compatibility — primary auth model is capabilities) |

## 5C. Data Structures

```c
// Process control block
typedef struct process_t {
    uint64_t pid;                      // process ID (unique)
    uint64_t parent_pid;               // creator's PID
    enum { ALIVE, ZOMBIE, TERMINATED } state;
    // Address space
    pagetable_t* page_table;            // top-level page table (CR3 value)
    // Threads
    thread_t**   threads;              // array of thread pointers
    uint32_t     thread_count;
    // Memory layout
    uintptr_t    code_base;
    uint64_t     code_size;
    uintptr_t    heap_base;
    uint64_t     heap_size;
    uintptr_t    stack_base;
    uint64_t     stack_size;
    // Capabilities
    capability_table_t* cap_table;     // capability tokens this process holds
    // Credentials
    uint32_t uid;
    uint32_t gid;
    // Signal state (minimal)
    uint64_t pending_signals;          // bitmask
    uint64_t blocked_signals;
    void (*signal_handlers[32])(void); // user-space handlers
    // Resource limits
    uint64_t max_memory_bytes;
    uint64_t max_threads;
    // Debug
    char name[256];
    // Exit status
    int exit_code;
} process_t;

// Mutex (with priority inheritance)
typedef struct mutex_t {
    volatile int locked;               // 0 = unlocked, 1 = locked (with waiters), owner = thread ID
    uint64_t      owner_tid;           // current owner
    wait_queue_t  wait_queue;          // threads waiting on this mutex
    uint32_t      original_priority;   // for priority inheritance restore
} mutex_t;

// Condition variable
typedef struct condvar_t {
    wait_queue_t wait_queue;
    uint32_t     num_waiters;
    spinlock_t   lock;                 // protects wait queue manipulation
} condvar_t;

// Capability token (unforgeable)
typedef struct cap_token_t {
    uint64_t id;                       // globally unique, randomly generated
    uint64_t object_id;                // what resource this grants access to (inode, device, memory region, etc.)
    cap_type_t type;                   // FILE_READ, FILE_WRITE, DEVICE_ACCESS, IPC_SEND, PROCESS_KILL, etc.
    uint64_t rights;                   // bitmask of granted operations
    uint64_t expires_at_ns;            // 0 = never expires
    uint32_t depth;                    // delegation depth (max 5)
    bool     revoked;                  // soft-delete for revocation
} cap_token_t;

// Capability table (per process)
typedef struct capability_table_t {
    cap_token_t* tokens;               // sorted array
    uint32_t     count;
    uint32_t     capacity;
    spinlock_t   lock;
    uint64_t     monotonic_id_counter; // for generating unique token IDs
} capability_table_t;

// Wait queue (used by sync primitives)
typedef struct wait_queue_t {
    thread_t**  waiters;               // array of blocked threads
    uint32_t    count;
    uint32_t    capacity;
    spinlock_t  lock;
    bool        priority_inheritance;  // if true, enable PI protocol
} wait_queue_t;
```

## 5D. API Surface (Downward Interface)

Exposed to Layer 4 (and Layer N-1 via syscall dispatch):

```
FUNCTION: process_spawn(const char* path, cap_token_t exec_cap, process_t** out) → err_t
PRECONDITIONS: path != NULL. exec_cap has FILE_EXECUTE right on path.
               Kernel heap has room for process_t.
POSTCONDITIONS: New process created with one thread (entry point from ELF binary).
                Address space set up but no memory mapped yet (Layer 4 handles mapping).
                Process added to process table. Returns ERR_CAP if exec_cap invalid.
SIDE EFFECTS: Process table updated. pid allocated from bitmap.

FUNCTION: process_exec(process_t* proc, const char* path, cap_token_t exec_cap) → err_t
PRECONDITIONS: proc != NULL. proc in ALIVE state.
POSTCONDITIONS: Address space replaced with new binary image.
                Stack and heap reset. Capabilities retained (or reset per policy).
SIDE EFFECTS: Old address space torn down via Layer 4 calls.

FUNCTION: process_exit(process_t* proc, int exit_code) → err_t
PRECONDITIONS: proc != NULL.
POSTCONDITIONS: Threads terminated. Resources not yet freed (ZOMBIE state until parent wait()).
                Exit code stored. Parent notified via event bus (PROCESS_DIED).
SIDE EFFECTS: Process state → ZOMBIE.

FUNCTION: process_wait(process_t* parent, uint64_t child_pid, int* exit_code_out) → err_t
PRECONDITIONS: parent != NULL. child_pid is a direct child of parent.
POSTCONDITIONS: If child alive, parent blocks until child exits.
                If child ZOMBIE, resources freed, exit_code written.
SIDE EFFECTS: Child process_t deallocated.

FUNCTION: process_kill(process_t* target, cap_token_t kill_cap) → err_t
PRECONDITIONS: target != NULL. kill_cap has PROCESS_KILL right for target.
POSTCONDITIONS: target forced to TERMINATED. All threads terminated.
SIDE EFFECTS: Event bus publishes PROCESS_DIED.

FUNCTION: thread_create(process_t* proc, uintptr_t entry, uintptr_t arg) → thread_t* | ERR
PRECONDITIONS: proc != NULL. proc->thread_count < proc->max_threads.
POSTCONDITIONS: New thread_t allocated. Kernel stack created.
                Thread added to proc->threads and to Layer 2 scheduler.
SIDE EFFECTS: Memory alloc for stack. Scheduler add_thread called.

FUNCTION: thread_exit(void) → void
PRECONDITIONS: Called from thread context.
POSTCONDITIONS: Thread removed from scheduler. If last thread in process, process_exit.
SIDE EFFECTS: sched_remove_thread, process->thread_count--

FUNCTION: thread_join(thread_t* t, int* exit_code_out) → err_t
PRECONDITIONS: t belongs to same process. t != current thread.
POSTCONDITIONS: Caller blocks until t exits. Exit code optionally returned.
SIDE EFFECTS: Thread resources freed after join.

FUNCTION: mutex_init(mutex_t* m) → err_t
PRECONDITIONS: m != NULL.
POSTCONDITIONS: Initialized unlocked.
SIDE EFFECTS: None.

FUNCTION: mutex_lock(mutex_t* m, uint64_t timeout_ms) → err_t
PRECONDITIONS: m initialized. Called from thread context.
POSTCONDITIONS: On success, mutex locked. If already locked, caller blocks.
                Priority inheritance: if current thread's priority > owner's, owner's priority boosted.
                On timeout, returns ERR_TIMEOUT.
SIDE EFFECTS: May trigger priority inheritance change.

FUNCTION: mutex_unlock(mutex_t* m) → err_t
PRECONDITIONS: m owned by current thread.
POSTCONDITIONS: Mutex unlocked. If waiters, one is woken.
                Owner's priority restored to original.
SIDE EFFECTS: sched_wake called for next waiter.

FUNCTION: condvar_wait(condvar_t* cv, mutex_t* m, uint64_t timeout_ms) → err_t
PRECONDITIONS: cv initialized. m locked by current thread.
POSTCONDITIONS: Mutex released. Thread blocks on cv. On wake, mutex re-acquired.
SIDE EFFECTS: Mutex unlock/relock.

FUNCTION: condvar_signal(condvar_t* cv) → err_t
PRECONDITIONS: cv initialized.
POSTCONDITIONS: One waiting thread woken (if any).
SIDE EFFECTS: sched_wake.

FUNCTION: condvar_broadcast(condvar_t* cv) → err_t
PRECONDITIONS: cv initialized.
POSTCONDITIONS: All waiting threads woken.
SIDE EFFECTS: sched_wake for all.

FUNCTION: capability_issue(process_t* issuer, cap_token_t parent_token,
                           cap_type_t type, uint64_t object_id, uint64_t rights) → cap_token_t | ERR
PRECONDITIONS: issuer != NULL. parent_token grants CAP_DELEGATE right.
               If parent_token.depth >= 5, delegation declined.
POSTCONDITIONS: New token created with depth = parent_token.depth + 1.
                Token added to issuer's cap table.
SIDE EFFECTS: Cap table updated.

FUNCTION: capability_validate(process_t* proc, cap_token_t token,
                              cap_type_t type, uint64_t rights_needed) → bool
PRECONDITIONS: proc != NULL.
POSTCONDITIONS: Returns true if token in proc's cap table, not revoked,
                type matches, rights are superset of needed, and not expired.
SIDE EFFECTS: None (pure check).

FUNCTION: capability_revoke(process_t* proc, uint64_t token_id) → err_t
PRECONDITIONS: proc owns the token.
POSTCONDITIONS: Token.revoked = true. Any derived tokens also revoked (recursive).
                Processes holding derived tokens get event bus notification (CAP_REVOKED).
SIDE EFFECTS: Event bus publish. Token table updated.
```

## 5E. Interaction Contract with Layer Below

**Expectations from Layer 2**:
- `sched_add_thread`, `sched_remove_thread`, `sched_block`, `sched_wake`
- `ipc_queue_create/destroy/send/recv` — for capability-mediated IPC
- `clock_sleep` — for timed waits
- `intr_disable/restore` — for short critical sections

**Expectations from Layer 4** (downward dependency for memory ops):
- `vm_map_region` — to set up address space for new process
- `vm_unmap_region` — to tear down address space on exec/exit
- `vm_alloc_pages` — for kernel stacks of new threads
- `vm_protect` — to set page permissions

**Guarantees to Layer 4 and Layer N-1**:
- Thread safety: `fork()` does not leave mutexes in broken state
- Capability isolation: a process cannot access another process's cap table
- No orphaned threads: when process exits, all threads are terminated
- Priority inheritance prevents priority inversion for all kernel mutexes
- Process IDs are recycled only after `wait()` collected exit status

## 5F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **Fork bomb (too many processes)** | Process table full → spawn returns ERR_PROC_LIMIT; OOM-killer at Layer 4 may reap lowest-priority process |
| **Thread limit exceeded** | `thread_create` returns ERR_THREAD_LIMIT; process cannot create more threads (configurable via `RLIMIT_NPROC`) |
| **Zombie flood (parent never waits)** | If parent dies first, children are re-parented to `init` process (PID 1); init loops on `wait` |
| **Mutex deadlock** | No kernel panic. Thread blocks forever. Watchdog detects thread blocked > 30s → deadlock diagnosis → kills one thread or escalates to SAFE mode |
| **Priority inversion** | Priority inheritance prevents: when high-priority thread blocks on mutex held by low-priority thread, low-priority thread inherits the high priority temporarily |
| **Capability token forgery** | Tokens are 64-bit random IDs from kernel CSPRNG; collision probability ~2^-64; kernel never exposes token IDs to user space (handles are hashed) |
| **Capability depth exhausted** | Maximum delegation depth = 5; attempt to delegate deeper returns ERR_CAP_DEPTH |
| **Revoke during operation** | Capability validated atomically at start of operation; operation completes with validated rights. New operations see revoked status |
| **Execve with open file handles** | File handles (Layer 5) with CLOEXEC flag are closed; non-CLOEXEC remain open |
| **Kill of already-zombie process** | No-op (ERR_ALREADY_ZOMBIE) |
| **Thread exits while holding mutex** | Mutex enters BROKEN state; future lock attempts return ERR_BROKEN_MUTEX; no deadlock |
| **Signal sent to process with no handler** | Default action: TERMINATE (for SIGKILL, SIGTERM) or IGNORE (for SIGCHLD) |

## 5G. Debuggability Interface

```c
err_t proc_trace(trace_verbosity_t v);
    // LOW:   log spawn/exec/exit/kill
    // MED:   add thread create/join/exit, capability issue/revoke
    // HIGH:  add every mutex lock/unlock, every cap validation

typedef enum { PROC_OK, PROC_DEGRADED, PROC_FAILED } proc_health_t;

proc_health_t proc_health_check(char* reason, size_t len);
    // Checks:
    //   - Process table: no process stuck in ALIVE > 1yr
    //   - Zombie count: < 100 (else init may be stuck)
    //   - Cap table: no token expires_in < 0 that hasn't been cleaned
    //   - Thread counts: sum(process.thread_count) matches Layer 2 runnable+blocked count

err_t proc_fault_inject(proc_fault_type_t t);
    // FAULT_FORK_FAIL:   every fork returns ERR_PROC_LIMIT
    // FAULT_THREAD_HANG: thread_create succeeds but thread immediately deadlocks
    // FAULT_CAP_LEAK:    capability_validate always returns true (even for revoked)
    // FAULT_MUTEX_BROKEN: next mutex_lock returns ERR_BROKEN_MUTEX

typedef struct {
    uint64_t spawns;
    uint64_t execs;
    uint64_t exits;
    uint64_t kills;
    uint64_t zombie_current;
    uint64_t processes_total;
    uint64_t threads_total;
    uint64_t mutex_locks;
    uint64_t mutex_contentions;     // lock had waiters
    uint64_t cap_issues;
    uint64_t cap_validations;
    uint64_t cap_revocations;
    uint64_t cap_denials;           // validation returned false
} proc_stats_t;

proc_stats_t proc_stats(void);
```

> **SMP Extension Notes**:
> - Process table protected by rwlock (readers don't block each other for `wait`/`getpid`)
> - `fork()` must synchronize with all threads in process (suspend them before cloning address space)
> - Mutex priority inheritance works cross-CPU: if mutex owner runs on CPU A and waiter on CPU B, IPI reschedule sent to CPU A

---

# 6. LAYER 4: MEMORY MANAGEMENT

## 6A. Layer Specification

**Purpose**: Manage all virtual and physical memory. Provide the abstraction of virtual address spaces, handle page faults, enforce memory protection, and manage the swap device.

**Responsibilities**:
- Physical page allocation (buddy allocator or slab)
- Virtual address space management per process (page tables)
- Memory-mapped files (`mmap`)
- Demand paging (lazy page allocation on first access)
- Swap: page eviction to block device, page reclaim
- Copy-on-Write (COW) fork support
- Memory protection: R/W/X page permissions, guard pages, stack guard
- Out-of-memory (OOM) killer
- Kernel heap (slab allocator) for kernel-internal allocations
- Page cache for file system (shared with Layer 5)

**Does NOT own**:
- Process lifecycle (Layer 3)
- File system metadata (Layer 5)
- User-level heap management (`malloc`/`free` are user-space, but backed by Layer 4 `brk`/`mmap`)

## 6B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `buddy_allocator` | Physical page allocator using buddy system; splits/coalesces free pages in power-of-2 sizes (4KB to 2MB); maintains free lists for each order |
| `slab_allocator` | Kernel heap for small objects (≤ 4KB); caches for `thread_t`, `process_t`, `file_t`, `cap_token_t`, etc. |
| `vm_mapper` | Manages page tables (per-process). Handles `map_page`, `unmap_page`, `page_table_walk`. Invalidates TLB entries. |
| `swap_manager` | Manages swap space on block device (partition or file). Handles page-out (eviction policy: clock/aging) and page-in (demand). |
| `oom_killer` | Activated when `buddy_allocator` fails to satisfy request. Selects victim process based on OOM score (rss + swap_used / priority). |
| `page_cache` | Caches file-backed pages (shared with Layer 5 VFS). Implements read-ahead and write-back. |
| `protection_enforcer` | Enforces page permissions, handles guard page detection, manages COW tracking. |

## 6C. Data Structures

```c
// Page table entry (x86-64 format)
typedef struct {
    uint64_t present      : 1;   // page in memory
    uint64_t rw           : 1;   // 0=read-only, 1=read-write
    uint64_t user         : 1;   // 0=supervisor, 1=user
    uint64_t write_through: 1;
    uint64_t cache_disabled: 1;
    uint64_t accessed     : 1;   // set by MMU on access
    uint64_t dirty        : 1;   // set by MMU on write
    uint64_t huge_page    : 1;   // 0=4KB, 1=2MB/1GB
    uint64_t global       : 1;   // don't flush TLB on CR3 reload
    uint64_t available    : 3;   // OS-defined
    uint64_t phys_addr    : 40;  // physical address of page or next table
    uint64_t reserved     : 11;
    uint64_t nx           : 1;   // no-execute
} __attribute__((packed)) page_table_entry_t;

// Buddy allocator state
typedef struct buddy_t {
    struct list_head free_lists[11]; // orders 0 (4KB) through 10 (2MB)
    spinlock_t       lock;
    uint64_t         total_pages;
    uint64_t         free_pages;
    uint64_t         min_free_pages;  // watermark for OOM
} buddy_t;

// Slab cache
typedef struct slab_cache_t {
    char     name[32];
    uint32_t object_size;       // size of objects in this cache
    uint32_t objects_per_slab;  // pages / object_size
    struct list_head slabs_full;
    struct list_head slabs_partial;
    struct list_head slabs_free;
    spinlock_t       lock;
    uint64_t         alloc_count;
    uint64_t         free_count;
} slab_cache_t;

// Virtual memory area (per-process mapping descriptor)
typedef struct vma_t {
    uintptr_t        start;          // virtual start address
    uintptr_t        end;            // virtual end address (exclusive)
    uint32_t         flags;          // READ, WRITE, EXEC, SHARED, ANONYMOUS
    uint32_t         prot;           // protection bits (matching page table)
    struct vma_t*    next;
    struct vma_t*    prev;
    // Backing (for file-backed mappings)
    file_t*          backing_file;   // NULL for anonymous
    uint64_t         file_offset;    // offset in backing file
    // COW tracking
    bool             cow;            // copy-on-write flag
    uint32_t         ref_count;      // shared mappings
} vma_t;

// Swap slot
typedef struct swap_slot_t {
    uint32_t slot_index;            // index on swap device
    // backlink
    uintptr_t virtual_addr;         // which vaddr this was paged from
    uint64_t  process_pid;          // owning process
} swap_slot_t;

// Swap bitmap
typedef struct swap_map_t {
    uint8_t*  bitmap;               // one bit per slot: 0=free, 1=used
    uint64_t  total_slots;          // swap device size / page size
    uint64_t  free_slots;
    spinlock_t lock;
} swap_map_t;

// Page cache entry
typedef struct page_cache_entry_t {
    uint64_t        inode_no;
    uint64_t        offset;         // file offset
    uintptr_t       phys_page;      // physical address of cached page
    bool            dirty;          // needs write-back
    bool            uptodate;       // content valid
    struct list_head lru_link;       // LRU chain for eviction
    spinlock_t      lock;
} page_cache_entry_t;

// OOM victim selector data
typedef struct oom_context_t {
    uint64_t trigger_count;
    uint64_t victims_killed;
    uint64_t last_trigger_ns;       // rate-limit OOM killing
    uint32_t min_ticks_between_kills; // avoid killing too fast
} oom_context_t;
```

## 6D. API Surface (Downward Interface)

Exposed to Layer 3 and Layer 5:

```
FUNCTION: vm_init(uint64_t mem_size, boot_info_t* boot) → err_t
PRECONDITIONS: Called once, early boot. hal_init done. Physical memory map known.
POSTCONDITIONS: Buddy allocator initialized over all usable RAM.
                Slab allocators created. Initial kernel heap set up.
                Identity mapping for kernel text/data in place.
SIDE EFFECTS: Page tables allocated. Permanent kernel mappings established.

FUNCTION: vm_alloc_pages(uint32_t order, uint32_t flags) → uintptr_t | ERR_OOM
PRECONDITIONS: order ∈ [0, 10]. flags may specify ZONE_DMA (if < 16MB).
POSTCONDITIONS: Returns physical address of contiguous 4KB*2^order page block.
                Pages zeroed unless NO_ZERO flag set.
SIDE EFFECTS: Buddy free list updated.

FUNCTION: vm_free_pages(uintptr_t phys_addr, uint32_t order) → err_t
PRECONDITIONS: phys_addr was returned by vm_alloc_pages with same order.
POSTCONDITIONS: Pages returned to buddy allocator.
SIDE EFFECTS: Buddy free list updated; coalescing may occur.

FUNCTION: vm_slab_create(const char* name, uint32_t obj_size) → slab_cache_t* | ERR
PRECONDITIONS: obj_size > 0 && obj_size <= 4096.
POSTCONDITIONS: New slab cache created; empty.
SIDE EFFECTS: Memory for cache descriptor allocated.

FUNCTION: vm_slab_alloc(slab_cache_t* cache) → void* | ERR_OOM
PRECONDITIONS: cache != NULL.
POSTCONDITIONS: Returns a zeroed object of cache->object_size bytes.
SIDE EFFECTS: May allocate new slab pages from buddy.

FUNCTION: vm_slab_free(slab_cache_t* cache, void* obj) → err_t
PRECONDITIONS: obj was returned by vm_slab_alloc from this cache.
POSTCONDITIONS: Object returned to slab. Slab page freed if entirely free.
SIDE EFFECTS: Slab list updated.

FUNCTION: vm_map_region(process_t* proc, vma_t* vma) → err_t
PRECONDITIONS: proc != NULL. vma describes valid virtual range.
               Range does not overlap existing vmas (caller checks).
POSTCONDITIONS: Page tables updated for the range. VMA added to process vma list.
                Pages are NOT allocated yet — demand paging will handle on fault.
SIDE EFFECTS: Page table entries created (initially not-present). TLB may need flush.

FUNCTION: vm_unmap_region(process_t* proc, uintptr_t start, uintptr_t end) → err_t
PRECONDITIONS: start, end page-aligned. Range is mapped.
POSTCONDITIONS: Page table entries cleared. Physical pages freed.
                VMA removed from process list.
SIDE EFFECTS: TLB flush on relevant CPU(s). Memory pressure reduced.

FUNCTION: vm_protect(process_t* proc, uintptr_t addr, uint64_t size,
                     uint32_t new_prot) → err_t
PRECONDITIONS: addr page-aligned. Range mapped.
POSTCONDITIONS: Page table entries' permission bits updated.
SIDE EFFECTS: TLB flush for the range.

FUNCTION: vm_page_fault_handler(process_t* proc, uintptr_t fault_addr,
                                uint32_t fault_flags) → err_t
PRECONDITIONS: Called from exception handler (Layer 2 interrupt dispatch).
               fault_flags: {PRESENT, WRITE, USER, NX, PROT}.
POSTCONDITIONS:
    - Page not present & valid VMA: allocate physical page, fill from swap/file if needed,
      map into page table, return to faulting instruction.
    - Page present & COW: copy page, update permissions, retry.
    - Invalid access (no VMA / protection violation): deliver SIGSEGV to process.
    - Swap: read page from swap device, update page table.
SIDE EFFECTS: Physical page allocated. May trigger eviction if low on memory.

FUNCTION: vm_handle_oom(void) → err_t
PRECONDITIONS: Called when vm_alloc_pages returns ERR_OOM.
POSTCONDITIONS: OOM killer selects victim. Victim process killed.
                Memory freed. Returns OK if enough freed.
SIDE EFFECTS: Process termination. Process table entry cleaned.

FUNCTION: vm_swap_out(process_t* proc, uintptr_t vaddr) → err_t
PRECONDITIONS: page at vaddr is pageable (not pinned, not kernel).
POSTCONDITIONS: Page content written to swap device. Page table entry marked not-present
                with swap offset encoded.
SIDE EFFECTS: Swap bitmap updated. Page freed to buddy.

FUNCTION: vm_swap_in(uintptr_t vaddr, swap_slot_t slot) → err_t
PRECONDITIONS: Slot valid. Physical page allocated.
POSTCONDITIONS: Data read from swap into allocated page. Page table entry restored.
SIDE EFFECTS: Swap bitmap cleared. Page marked dirty? (no, clean as read from swap).

FUNCTION: vm_page_cache_lookup(uint64_t inode_no, uint64_t offset) → page_cache_entry_t* | NULL
PRECONDITIONS: None.
POSTCONDITIONS: Returns cached page if present, else NULL.
SIDE EFFECTS: LRU update.

FUNCTION: vm_page_cache_insert(uint64_t inode_no, uint64_t offset,
                               uintptr_t phys_page) → page_cache_entry_t*
PRECONDITIONS: phys_page valid. Not already cached.
POSTCONDITIONS: Entry added to cache.
SIDE EFFECTS: LRU updated. May evict oldest entry.

FUNCTION: vm_get_stats(vm_stats_t* out) → err_t
PRECONDITIONS: out != NULL.
POSTCONDITIONS: out filled with current memory statistics.
SIDE EFFECTS: None.
```

## 6E. Interaction Contract with Layer Below

**Expectations from Layer 3**:
- `process_t` structure with valid `page_table` pointer for address space operations
- `process_spawn/exec/exit/kill` — so that Layer 4 knows when to allocate/tear down address spaces
- Notification when process dies (via event bus) so page cache entries for that process can be freed (if not shared)

**Expectations from Layer 5**:
- `file_read`/`file_write` — for page cache read-ahead and write-back
- Block device access for swap I/O

**Expectations from Layer 1**:
- `hal_phys_alloc_frame` — used by buddy allocator for initial bootstrap pages before buddy is set up
- `hal_mmio_map` — for device memory

**Guarantees to Layer 3 and Layer 5**:
- Every allocation returns zeroed pages (no data leak between processes)
- Contiguous physical pages for DMA (up to 2MB; larger needs scatter-gather)
- Demand paging is transparent — callers map regions and access memory normally
- OOM killing only targets user processes, never kernel threads
- Page cache coherency: read returns latest written data (write-back before eviction)

## 6F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **OOM (allocation fails)** | Buddy allocator triggers `vm_handle_oom`. OOM killer selects victim by score (worst offenders: RSS + swap_used). If no victim found (all kernel threads), system enters SAFE mode. If still OOM in SAFE mode, enters EMERGENCY mode. |
| **OOM killer selects init** | `init` process is exempt (PID 1). If init is the only process, panic with "Out of memory and no killable process." |
| **Page fault on kernel address** | If fault_addr >= KERNEL_BASE, it's a kernel bug. Panic with dump. |
| **Double page fault (page fault in handler)** | CPU handles via double fault (#DF) IST. Kernel panics with "Recursive page fault." |
| **Swap device full** | `vm_swap_out` returns ERR_NOSPACE. Page stays in memory. OOM killer more aggressive. On EMERGENCY mode, swap disabled. |
| **Swap I/O error during page-in** | Page remains not-present. Process receives SIGBUS (not SIGSEGV). |
| **COW page fault on shared anonymous mapping** | For MAP_SHARED | MAP_ANONYMOUS, no copy: page is writable by all sharers. Only MAP_PRIVATE triggers COW. |
| **TLB not flushed after unmap** | (Bug scenario.) Architecture forces TLB shootdown on SMP. On uniprocessor, `vm_unmap_region` issues `invlpg` for each page. |
| **Process allocates memory until OOM then is killed** | Process termination in OOM killer calls `vm_unmap_region` for all vmas → pages freed → memory pressure relieved. |
| **Kernel heap fragmentation** | Slab allocator partially mitigates. Buddy allocator can compact if built with compaction support. If fragmentation > 90%, log warning. |
| **Page cache thrashing** | Too many page-ins/page-outs (high major fault rate). OOM killer or swap token (only one process allowed to page-in at a time) prevents thrash. |
| **Large allocation > 2MB** | Falls through to full page table walk; buddy allocator cannot coalesce beyond 2MB. For larger, use vmalloc-like segmented mapping. |
| **Memory leak in driver** | Driver memory not freed → slab allocator tracks per-module allocation count → on module unload, leaked pages detected and warned |

## 6G. Debuggability Interface

```c
err_t mem_trace(trace_verbosity_t v);
    // LOW:   log major events: OOM, OOM kill, swap out/in, large allocs
    // MED:   add page fault type breakdown, alloc/free per slab cache
    // HIGH:  add every page table walk result, every TLB flush, every page cache eviction

typedef enum { MEM_OK, MEM_DEGRADED, MEM_FAILED } mem_health_t;

mem_health_t mem_health_check(char* reason, size_t len);
    // Checks:
    //   - Free pages above watermark (min_free_pages)?
    //   - Swap slots available > 10%?
    //   - Buddy allocator: free_pages + allocated_pages == total_pages?
    //   - Slab caches: no object_count exceeds slab pages * objects_per_slab?
    //   - Page cache: total cached pages less than 80% of available RAM?
    //   - No process with vm_map count > 10000 (potential leak)?

err_t mem_fault_inject(mem_fault_type_t t);
    // FAULT_OOM_EVERY:  every vm_alloc_pages returns ERR_OOM
    // FAULT_PF_RANDOM:  1% of page faults deliver SIGSEGV even for valid access
    // FAULT_SWAP_FAIL:  vm_swap_in always returns ERR_IO
    // FAULT_SLAB_CORRUPT: slab allocator returns overlapping objects

typedef struct {
    uint64_t total_physical_pages;
    uint64_t free_pages;
    uint64_t kernel_pages;          // allocated to kernel
    uint64_t user_pages;            // allocated to user processes
    uint64_t page_cache_pages;
    uint64_t swap_total_slots;
    uint64_t swap_used_slots;
    uint64_t page_faults_total;
    uint64_t page_faults_minor;     // page already in page cache
    uint64_t page_faults_major;     // required disk I/O
    uint64_t page_faults_segv;      // invalid access
    uint64_t oom_events;
    uint64_t oom_victims;
    uint64_t slab_cache_count;
    // Per-slab breakdown (top 5 by usage)
    struct {
        char     name[32];
        uint64_t objects_total;
        uint64_t objects_active;
        uint64_t memory_pages;
    } slab_stats[5];
} mem_stats_t;

mem_stats_t mem_stats(void);
```

> **SMP Extension Notes**:
> - Each CPU has a per-CPU page allocator cache (hot pages) to reduce buddy lock contention
> - TLB shootdown: `vm_unmap_region` sends IPI to all CPUs running any thread of the affected process
> - Page table walks must be atomic with respect to concurrent page faults on other CPUs (use per-page-table lock)
> - Buddy allocator: per-CPU free lists + a global pool; periodic rebalancing

---

# 7. LAYER 5: I/O, FILE SYSTEM & DEVICE MANAGEMENT

## 7A. Layer Specification

**Purpose**: Provide a unified, hierarchical file system abstraction over block storage, device nodes, and virtual file systems. Manage I/O operations with buffering and caching.

**Responsibilities**:
- Virtual File System (VFS) — unified namespace for all file system types
- File system drivers: ext2 (on disk), devfs (device nodes), tmpfs (RAM disk), procfs (process info)
- File operations: open, close, read, write, seek, truncate, fsync
- Directory operations: mkdir, rmdir, readdir, link, unlink, rename, stat
- File descriptor management per process
- Block device caching (page cache integration with Layer 4)
- Device nodes: /dev/uart, /dev/sda, etc.
- Mount management: mount, unmount

**Does NOT own**:
- Physical page allocation (Layer 4 — owns the page cache)
- Process lifecycle (Layer 3)
- Virtual memory mapping (Layer 4 — but VFS provides file backing for mmap)

## 7B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `vfs` | Inode cache, dentry cache, mount table, path resolution (walking `/a/b/c` to inode), superblock management |
| `fd_manager` | Per-process file descriptor table (fd → `file_t*` mapping), fd allocation, fd inheritance on fork |
| `ext2` | ext2 file system driver: reads/writes inode tables, block bitmaps, directory entries |
| `devfs` | Pseudo-fs for device nodes: `/dev/null`, `/dev/zero`, `/dev/uart`, `/dev/sda`, `/dev/timer` |
| `tmpfs` | RAM-backed file system: inodes and data in kernel memory (via slab allocator) |
| `procfs` | Pseudo-fs exposing process info: `/proc/<pid>/status`, `/proc/<pid>/maps` |
| `block_cache` | Caches block data (a subset of page cache specialized for raw block I/O); write-back daemon |
| `device_manager` | Maintains device ID mapping (major/minor numbers); dispatches read/write to device drivers |

## 7C. Data Structures

```c
// Inode (on-disk + in-memory)
typedef struct inode_t {
    uint64_t    inode_no;            // unique within file system
    uint16_t    mode;                // file type + permissions (S_IFREG, S_IFDIR, etc.)
    uint32_t    uid;
    uint32_t    gid;
    uint64_t    size;
    uint64_t    blocks;              // number of 512B blocks
    uint64_t    atime;               // access time (monotonic ns)
    uint64_t    mtime;               // modify time
    uint64_t    ctime;               // change time
    uint32_t    link_count;
    // Block pointers (ext2-style)
    uint32_t    direct_blocks[12];
    uint32_t    indirect_block;
    uint32_t    double_indirect;
    uint32_t    triple_indirect;
    // In-memory only
    spinlock_t  lock;
    uint32_t    ref_count;           // number of open references
    bool        dirty;               // needs write-back
    bool        mount_point;         // is this a mount point?
    // File system backlink
    struct fs_driver_t* fs;
} inode_t;

// Dentry (directory entry cache)
typedef struct dentry_t {
    char            name[256];       // component name
    uint64_t        parent_inode_no;
    uint64_t        inode_no;
    struct dentry_t *parent;
    struct dentry_t *child;          // first child
    struct dentry_t *sibling;        // next sibling
    uint32_t        ref_count;
    bool            negative;        // cached miss (name does not exist)
} dentry_t;

// Open file description
typedef struct file_t {
    inode_t*    inode;
    uint64_t    file_pos;            // current read/write position
    uint32_t    flags;               // O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_NONBLOCK
    cap_token_t access_cap;          // capability used to open this file
    struct file_operations_t* ops;   // function table (read, write, lseek, etc.)
    void*       private_data;        // driver-specific (e.g., device ID for devfs)
    spinlock_t  lock;
    uint32_t    ref_count;
} file_t;

// File descriptor (per-process)
typedef struct fd_entry_t {
    file_t* file;
    uint32_t fd_flags;               // FD_CLOEXEC
} fd_entry_t;

typedef struct fd_table_t {
    fd_entry_t* entries;
    uint32_t    capacity;            // grows as needed
    uint32_t    count;
    spinlock_t  lock;
} fd_table_t;

// Mount point
typedef struct mount_t {
    char     source[256];            // device path (e.g., "/dev/sda1") or "none"
    char     target[256];            // mount point (e.g., "/mnt")
    char     fstype[16];             // "ext2", "tmpfs", "devfs", "procfs"
    inode_t* mount_point_inode;      // inode of target directory
    inode_t* root_inode;             // root inode of mounted fs
    struct mount_t* next;
} mount_t;

// VFS superblock (per file system instance)
typedef struct superblock_t {
    uint64_t    fs_id;
    char        fstype[16];
    inode_t*    root_inode;
    uint64_t    block_size;
    uint64_t    total_blocks;
    uint64_t    free_blocks;
    uint64_t    total_inodes;
    uint64_t    free_inodes;
    struct fs_driver_t* driver;
    void*       private_data;        // fs-specific (e.g., ext2 superblock)
} superblock_t;

// File system driver interface
typedef struct fs_driver_t {
    char name[16];
    err_t (*mount)(superblock_t* sb, void* device_handle);
    err_t (*unmount)(superblock_t* sb);
    inode_t* (*inode_read)(superblock_t* sb, uint64_t inode_no);
    err_t    (*inode_write)(superblock_t* sb, inode_t* inode);
    err_t    (*block_read)(superblock_t* sb, uint64_t block_no, void* buffer);
    err_t    (*block_write)(superblock_t* sb, uint64_t block_no, const void* buffer);
    err_t    (*readdir)(inode_t* dir, uint32_t index, char* name, uint64_t* inode_no);
    err_t    (*link)(inode_t* dir, const char* name, inode_t* inode);
    err_t    (*unlink)(inode_t* dir, const char* name);
    err_t    (*mkdir)(inode_t* dir, const char* name, uint16_t mode);
    err_t    (*rmdir)(inode_t* dir, const char* name);
    err_t    (*truncate)(inode_t* inode, uint64_t new_size);
} fs_driver_t;

// Device node descriptor
typedef struct device_t {
    uint16_t major;
    uint16_t minor;
    char     name[64];
    struct file_operations_t* ops;   // open, read, write, ioctl
    void*    private_data;
    struct device_t* next;
} device_t;

// File operations (polymorphic)
typedef struct file_operations_t {
    err_t (*open)(file_t* file, uint32_t flags);
    err_t (*close)(file_t* file);
    err_t (*read)(file_t* file, void* buf, uint64_t count, uint64_t* bytes_read);
    err_t (*write)(file_t* file, const void* buf, uint64_t count, uint64_t* bytes_written);
    err_t (*lseek)(file_t* file, int64_t offset, uint32_t whence, uint64_t* new_pos);
    err_t (*ioctl)(file_t* file, uint32_t request, void* arg);
    err_t (*mmap)(file_t* file, vma_t* vma);
    err_t (*fsync)(file_t* file);
} file_operations_t;
```

## 7D. API Surface (Downward Interface)

Exposed to Layer N-1 (syscall gateway):

```
FUNCTION: vfs_init(void) → err_t
PRECONDITIONS: vm_init done. Slab allocators for inode/dentry/file available.
POSTCONDITIONS: Root mount table initialized. devfs mounted on /dev.
                tmpfs mounted on /tmp. procfs mounted on /proc.
                /dev/uart, /dev/null, /dev/zero created.
SIDE EFFECTS: Mount table populated.

FUNCTION: vfs_open(process_t* proc, const char* path, uint32_t flags,
                   cap_token_t cap, file_t** out) → err_t
PRECONDITIONS: path != NULL. cap has FILE_READ or FILE_WRITE right for path.
               proc has fd_table initialized.
POSTCONDITIONS: If success: *out points to opened file_t. File position at 0.
                If O_CREAT and file does not exist, file created.
                If error: *out = NULL, error code returned.
SIDE EFFECTS: inode ref_count++. dentry cache updated.

FUNCTION: vfs_close(process_t* proc, file_t* file) → err_t
PRECONDITIONS: file != NULL. file->ref_count > 0.
POSTCONDITIONS: file->ref_count--. If 0, inode->ref_count-- (fsync implied if dirty).
                Process fd_table cleared.
SIDE EFFECTS: Dirty data written if last reference.

FUNCTION: vfs_read(file_t* file, void* buf, uint64_t count, uint64_t* bytes_read) → err_t
PRECONDITIONS: file opened with O_RDONLY or O_RDWR. file->file_pos valid.
POSTCONDITIONS: *bytes_read = actual bytes read (may be < count at EOF).
                file->file_pos advanced.
SIDE EFFECTS: Page cache may trigger page-in. Block I/O may occur.

FUNCTION: vfs_write(file_t* file, const void* buf, uint64_t count, uint64_t* bytes_written) → err_t
PRECONDITIONS: file opened with O_WRONLY or O_RDWR. buf != NULL.
POSTCONDITIONS: *bytes_written = actual bytes written (may be < count if disk full).
                file->file_pos advanced. inode marked dirty.
SIDE EFFECTS: Page cache dirty page created, write-back may be scheduled.

FUNCTION: vfs_lseek(file_t* file, int64_t offset, uint32_t whence, uint64_t* new_pos) → err_t
PRECONDITIONS: file != NULL. whence ∈ {SEEK_SET, SEEK_CUR, SEEK_END}.
POSTCONDITIONS: *new_pos = resulting absolute position.
SIDE EFFECTS: None (no I/O).

FUNCTION: vfs_mkdir(const char* path, uint16_t mode, cap_token_t cap) → err_t
PRECONDITIONS: path is valid. parent directory exists. cap has DIR_WRITE on parent.
POSTCONDITIONS: New directory entry created.
SIDE EFFECTS: inode allocated. Dentry updated.

FUNCTION: vfs_readdir(file_t* dir, vfs_dirent_t* entries, uint32_t count,
                      uint32_t* read_count) → err_t
PRECONDITIONS: dir points to a directory inode.
POSTCONDITIONS: entries filled (up to count entries). read_count = actual.
SIDE EFFECTS: None.

FUNCTION: vfs_link(const char* oldpath, const char* newpath, cap_token_t cap) → err_t
PRECONDITIONS: oldpath exists. newpath doesn't exist. cap has FILE_WRITE on newpath's parent dir.
POSTCONDITIONS: Hard link created. inode->link_count++.
SIDE EFFECTS: Dentry added.

FUNCTION: vfs_unlink(const char* path, cap_token_t cap) → err_t
PRECONDITIONS: path exists. cap has FILE_WRITE on parent directory.
POSTCONDITIONS: Dentry removed. If inode->link_count == 1, inode marked for deletion on last close.
SIDE EFFECTS: Dentry cache invalidated.

FUNCTION: vfs_rename(const char* oldpath, const char* newpath, cap_token_t cap) → err_t
PRECONDITIONS: oldpath exists. newpath not exist (or is same inode).
POSTCONDITIONS: Dentry moved.
SIDE EFFECTS: Dentry cache updated.

FUNCTION: vfs_stat(const char* path, stat_t* buf, cap_token_t cap) → err_t
PRECONDITIONS: path exists. cap has FILE_READ right.
POSTCONDITIONS: buf filled with inode metadata.
SIDE EFFECTS: None.

FUNCTION: vfs_mount(const char* source, const char* target,
                    const char* fstype, uint32_t flags) → err_t
PRECONDITIONS: target exists and is a directory. fstype driver registered.
POSTCONDITIONS: New file system attached at target.
SIDE EFFECTS: Mount table entry added.

FUNCTION: vfs_umount(const char* target) → err_t
PRECONDITIONS: target is a mount point. No open files on mounted fs.
POSTCONDITIONS: File system synced and detached.
SIDE EFFECTS: Mount table entry removed.

FUNCTION: vfs_ioctl(file_t* file, uint32_t request, void* arg) → err_t
PRECONDITIONS: file != NULL. Device supports ioctl.
POSTCONDITIONS: Device-specific operation performed.
SIDE EFFECTS: Device-dependent.

FUNCTION: vfs_fsync(file_t* file) → err_t
PRECONDITIONS: file != NULL.
POSTCONDITIONS: All dirty pages for this file written to disk. inode written.
SIDE EFFECTS: Block I/O.

FUNCTION: vfs_sync(void) → err_t
PRECONDITIONS: None.
POSTCONDITIONS: All dirty inodes and pages across all mounted FS written to disk.
SIDE EFFECTS: Massive block I/O.

FUNCTION: device_register(device_t* dev) → err_t
PRECONDITIONS: dev != NULL. major/minor unique.
POSTCONDITIONS: Device added to device table. /dev/<name> created.
SIDE EFFECTS: devfs inode created.

FUNCTION: device_unregister(device_t* dev) → err_t
PRECONDITIONS: dev registered. No open handles.
POSTCONDITIONS: Device removed. /dev/<name> unlinked.
SIDE EFFECTS: devfs inode removed.

FUNCTION: fd_install(process_t* proc, file_t* file, uint32_t* fd_out) → err_t
PRECONDITIONS: proc != NULL. file != NULL.
POSTCONDITIONS: fd_out = lowest available fd number. fd_table entry installed.
SIDE EFFECTS: fd_table expanded if needed.

FUNCTION: fd_close(process_t* proc, uint32_t fd) → err_t
PRECONDITIONS: fd < proc->fd_table.capacity, fd is open.
POSTCONDITIONS: fd_table entry cleared. file->ref_count--.
SIDE EFFECTS: May trigger vfs_close if last reference.
```

## 7E. Interaction Contract with Layer Below

**Expectations from Layer 4**:
- `vm_page_cache_lookup/insert` — for file data caching
- `vm_alloc_pages` — for driver DMA buffers
- `vm_map_region` — for memory-mapped files

**Expectations from Layer 3**:
- `process_t` structure with `fd_table`, `uid`, `gid`
- Capability validation for file access

**Guarantees to Layer N-1**:
- Path resolution is race-free: all components locked via dentry cache spinlock
- Writes are buffered but `fsync` guarantees durable storage (if underlying media is reliable)
- Directory operations are atomic: `rename` cannot leave namespace in inconsistent state
- Device nodes are type-safe: writing to `/dev/null` always works; writing to `/proc` read-only files returns ERR_PERM
- File descriptors are inherited on fork (with CLOEXEC handling)

## 7F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **Disk full** | `vfs_write` returns ERR_NOSPACE (ENOSPC). Data not lost (write was not committed). Partial write returns bytes written before ENOSPC. |
| **Disk I/O error on read** | `vfs_read` returns ERR_IO. Page cache entry marked FAULTED. Subsequent reads also fail. Process receives SIGBUS if accessing mmap region. |
| **Disk I/O error on write** | Return ERR_IO. Dirty page stays dirty (may retry on flush). If write-back daemon persistently fails, FS remounted read-only in SAFE mode. |
| **File descriptor leak** | FD table grows until process limit (default 1024). `fd_install` returns ERR_PROC_LIMIT. |
| **Path too long** | PATH_MAX = 4096. Returns ERR_NAMETOOLONG (ENAMETOOLONG). |
| **Symlink loop** | Maximum symlink resolution depth = 40. Returns ERR_LOOP (ELOOP). |
| **Delete open file** | inode not freed until last file descriptor closed. On last close, inode deleted if link_count == 0. |
| **Concurrent read/write same file** | `file_t` lock serializes operations per file descriptor. Different fds on same file are independently positioned (but write to same inode is serialized by inode lock). |
| **Unmount with open files** | `vfs_umount` returns ERR_BUSY (EBUSY). Lazy unmount (MNT_DETACH) force-unmounts; subsequent ops on orphaned files return ERR_STALE. |
| **Corrupted inode on disk** | ext2 driver validates checksum; on mismatch, returns ERR_FS_CORRUPT; filesystem may need fsck. |
| **Race in concurrent mkdir same path** | The second mkdir fails with ERR_EXIST (EEXIST) — checked under parent directory lock. |
| **Power loss mid-write** | WAL in cross-cutting layer guarantees metadata consistency. Data pages may be lost (caller must fsync for durability). |
| **/dev/zero read at EOF** | /dev/zero has no EOF; reads return infinite zeroes. |

## 7G. Debuggability Interface

```c
err_t fs_trace(trace_verbosity_t v);
    // LOW:   log mount/umount, filesystem errors
    // MED:   add open/close per file, mkdir/rmdir/link/unlink
    // HIGH:  add every read/write/lseek, all path resolution steps, all dentry lookups

typedef enum { FS_OK, FS_DEGRADED, FS_FAILED } fs_health_t;

fs_health_t fs_health_check(char* reason, size_t len);
    // Checks:
    //   - All mounted filesystems can read root inode
    //   - Block device not returning persistent errors
    //   - Free blocks > 5% on root fs
    //   - Dentry cache not exhausted
    //   - Write-back daemon is alive and completing flushes
    //   - No inode with ref_count in millions (potential leak)

err_t fs_fault_inject(fs_fault_type_t t);
    // FAULT_DISK_FULL:     next write returns ERR_NOSPACE
    // FAULT_IO_ERROR:      next read/write returns ERR_IO
    // FAULT_CORRUPT_INODE: inode metadata reads return garbage
    // FAULT_DENTRY_MISS:   path resolution always fails with ENOENT

typedef struct {
    uint64_t mounted_fs_count;
    uint64_t open_files;
    uint64_t total_inodes_cached;
    uint64_t total_dentries_cached;
    uint64_t reads;
    uint64_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t block_reads;           // actual block I/O (not cached)
    uint64_t block_writes;
    uint64_t errors;
    uint64_t fsyncs;
    // per-fs breakdown
    struct {
        char     fstype[16];
        uint64_t free_blocks;
        uint64_t free_inodes;
        uint64_t cached_pages;
    } fs_stats[8];                  // up to 8 mounted filesystems
} fs_stats_t;

fs_stats_t fs_stats(void);
```

> **SMP Extension Notes**:
> - Inode cache uses RCU (Read-Copy-Update) for lock-free reads
> - Dentry cache is per-CPU with lazy reclamation
> - Write-back daemon is a dedicated thread that flushes dirty pages from all filesystems
> - Block cache: shared across all CPUs with per-bucket locking (hash on block number)

---

# 8. LAYER N-1: SYSCALL GATEWAY + SHELL INTERFACE

## 8A. Layer Specification

**Purpose**: Provide the sole entry point for user programs into the kernel. Every user-to-kernel transition must pass through this layer. It validates, audits, rate-limits, and dispatches system calls.

**Responsibilities**:
- System call dispatch: receive syscall number + arguments, validate, route to handler
- Capability verification on every syscall (with kernel-level token)
- Syscall audit logging: timestamp, process ID, capability token, arguments
- Rate limiting: per-process tokens (leaky bucket), circuit breaker for anomalies
- Signal delivery: deliver pending signals to user threads on kernel→user return
- Shell interface: command-line editing, job control, process launching for interactive use

**Does NOT own**:
- File system internals (Layer 5)
- Memory management internals (Layer 4)
- Process creation internals (Layer 3 — but dispatches the syscall to it)

## 8B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `syscall_dispatcher` | Large switch/table: maps syscall number → handler function pointer. Handles argument validation (copy from user-space). |
| `audit_logger` | Writes syscall records to a kernel log buffer (configurable size, circular). Records: timestamp, PID, capability token hash, syscall number, args, return code, latency. |
| `rate_limiter` | Per-process token bucket: N tokens/sec, burst up to M tokens. If bucket empty, syscall is delayed (not killed). If delay > threshold, circuit breaker activates. |
| `circuit_breaker` | Monitors per-process error rate and syscall frequency. If anomaly score > threshold, process is sandboxed (caps reduced, priority lowered, rate limited harder). |
| `signal_delivery` | On syscall return (or timer tick), checks pending signals for current process. Delivers signal by manipulating user stack (saving context, calling handler). |
| `shell` | Interactive command interpreter: built-in commands (ls, cat, echo, ps, kill), job control (foreground/background), pipe chaining, I/O redirection. Launches user programs via `spawn` syscall. |

## 8C. Data Structures

```c
// Syscall dispatch entry
typedef struct syscall_entry_t {
    uint32_t number;
    const char* name;
    err_t (*handler)(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                     uint64_t arg3, uint64_t arg4, uint64_t arg5,
                     uint64_t* result);
    uint32_t min_caps[6];           // required capability for each argument (0 if none)
    bool     audit;                 // always audit this call?
    uint32_t rate_limit_cost;       // tokens consumed from bucket
} syscall_entry_t;

// Audit record (fixed-size, pre-allocated)
typedef struct audit_record_t {
    uint64_t timestamp_ns;
    uint64_t pid;
    uint64_t tid;
    uint64_t syscall_number;
    uint64_t args[6];
    uint64_t result;
    uint64_t latency_us;            // time from entry to return
    cap_token_hash_t cap_hash;      // hash of capability token used (or 0)
    uint32_t cpu_id;
    enum { AUDIT_PASS, AUDIT_DENY_CAP, AUDIT_DENY_RATE } verdict;
} __attribute__((packed)) audit_record_t;

// Token bucket (per-process rate limiting)
typedef struct token_bucket_t {
    uint64_t tokens_per_sec;
    uint64_t burst_size;
    uint64_t current_tokens;        // atomic (fixed-point: 1 token = 1000 nanotokens)
    uint64_t last_refill_ns;        // last time tokens were added
    uint64_t max_queue_delay_ns;    // if delay exceeds this, circuit breaker triggered
} token_bucket_t;

// Circuit breaker state (per-process)
typedef struct circuit_breaker_t {
    uint32_t error_count;           // recent errors (decays over time)
    uint32_t anomaly_score;         // composite: error_rate + syscall_frequency_variance
    enum { CB_CLOSED, CB_HALF_OPEN, CB_OPEN } state;
    uint64_t last_trip_ns;          // when breaker last opened
    uint64_t cooldown_ns;           // how long before auto-reset (default: 10s)
} circuit_breaker_t;

// Signal frame (pushed onto user stack when delivering signal)
typedef struct signal_frame_t {
    uint64_t return_addr;           // address to resume after handler
    uint64_t saved_rsp;
    uint64_t saved_rflags;
    // saved registers (enough for full restore)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint32_t signal_num;
    uint32_t flags;
} signal_frame_t;
```

## 8D. API Surface (Downward Interface)

Exposed to Layer N (User Programs):

```
System Call Table (example — 32 syscalls defined):

NR=0:  sys_exit(int exit_code) → never returns
NR=1:  sys_spawn(const char* path, cap_token_t exec_cap, pid_t* out) → err_t
NR=2:  sys_wait(pid_t pid, int* exit_code) → err_t
NR=3:  sys_kill(pid_t pid, int signal, cap_token_t kill_cap) → err_t
NR=4:  sys_open(const char* path, uint32_t flags, cap_token_t cap, fd_t* out) → err_t
NR=5:  sys_read(fd_t fd, void* buf, uint64_t count, uint64_t* bytes_read) → err_t
NR=6:  sys_write(fd_t fd, const void* buf, uint64_t count, uint64_t* bytes_written) → err_t
NR=7:  sys_close(fd_t fd) → err_t
NR=8:  sys_lseek(fd_t fd, int64_t offset, uint32_t whence, uint64_t* new_pos) → err_t
NR=9:  sys_mmap(void* addr, uint64_t length, uint32_t prot, uint32_t flags,
                fd_t fd, uint64_t offset, void** out) → err_t
NR=10: sys_munmap(void* addr, uint64_t length) → err_t
NR=11: sys_brk(void* new_brk, void** out_brk) → err_t
NR=12: sys_getpid(pid_t* out) → err_t
NR=13: sys_clock_gettime(clockid_t clk, timespec_t* ts) → err_t
NR=14: sys_nanosleep(timespec_t* req, timespec_t* rem) → err_t
NR=15: sys_sched_yield(void) → err_t
NR=16: sys_thread_create(uintptr_t entry, uintptr_t arg, tid_t* out) → err_t
NR=17: sys_thread_join(tid_t tid, int* exit_code) → err_t
NR=18: sys_mutex_lock(mutex_user_t* m, uint64_t timeout_ms) → err_t
NR=19: sys_mutex_unlock(mutex_user_t* m) → err_t
NR=20: sys_condvar_wait(condvar_user_t* cv, mutex_user_t* m, uint64_t timeout_ms) → err_t
NR=21: sys_condvar_signal(condvar_user_t* cv) → err_t
NR=22: sys_mkdir(const char* path, uint16_t mode, cap_token_t cap) → err_t
NR=23: sys_readdir(fd_t dir, dirent_t* entries, uint32_t count, uint32_t* read_count) → err_t
NR=24: sys_link(const char* oldpath, const char* newpath, cap_token_t cap) → err_t
NR=25: sys_unlink(const char* path, cap_token_t cap) → err_t
NR=26: sys_rename(const char* oldpath, const char* newpath, cap_token_t cap) → err_t
NR=27: sys_stat(const char* path, stat_t* buf, cap_token_t cap) → err_t
NR=28: sys_cap_issue(cap_token_t parent, uint32_t type, uint64_t object_id,
                     uint64_t rights, cap_token_t* out) → err_t
NR=29: sys_cap_revoke(uint64_t token_id) → err_t
NR=30: sys_ioctl(fd_t fd, uint32_t request, void* arg) → err_t
NR=31: sys_fsync(fd_t fd) → err_t
```

## 8E. Interaction Contract with Layer Below

**Expectations from Layer 3, 4, 5**:
- All the APIs defined in previous sections (process lifecycle, memory management, file system)
- Layers must validate capabilities before acting on syscall requests

**Guarantees to Layer N (User Programs)**:
- Every syscall returns within bounded time (no infinite kernel-space loops)
- Arguments are validated (kernel never trusts user pointers)
- User data is copied safely: `copy_from_user` and `copy_to_user` with page fault recovery
- Syscall handler runs at interrupt level (or with interrupts enabled depending on design)
- Return to user is via `iretq` (or `sysretq`), restoring all user registers

## 8F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **User passes NULL pointer** | `copy_from_user` returns ERR_FAULT; syscall returns ERR_FAULT; process may get SIGSEGV |
| **User passes invalid fd** | `sys_read` with fd=9999 → lookup fails → returns ERR_BAD_FD (EBADF) |
| **Syscall number out of range** | Dispatcher returns ERR_NOSYS (ENOSYS); audit logged |
| **Rate limit exceeded** | Token bucket empty → syscall delayed (spins briefly or yields). If accumulated delay > 10ms, circuit breaker opens → process sandboxed |
| **Circuit breaker open** | Breaker stays open for 10s; affected process has: priority lowered to 0, capability scope reduced (no new file opens), rate limit cut to 1/100th. After cooldown, half-open: one test syscall allowed; if passes, breaker closes |
| **Audit log full** | Circular buffer overwrites oldest record. `sys_log_overflow` counter incremented |
| **Signal delivered to thread in syscall** | Signal delivery deferred until syscall returns (interruptible syscalls like `nanosleep` return ERR_CANCELED with partial result) |
| **Kernel stack overflow in syscall** | Guard page at bottom of kernel stack → page fault → double fault → panic. Prevented by generous stack size (16KB per thread) |
| **User program in infinite syscall loop** | Rate limiter slows it; circuit breaker opens; watchdog may kill process |
| **Shell: pipe buffer full** | `write` to pipe blocks (if O_NONBLOCK not set). Pipe buffer in Layer 2 IPC layer. |
| **Shell: background job exits** | Shell receives SIGCHLD, prints "[1]+ Done" on next prompt |

## 8G. Debuggability Interface

```c
err_t gate_trace(trace_verbosity_t v);
    // LOW:   log every syscall entry/exit (number, PID, return code)
    // MED:   add argument dumps (first 64 bytes of each pointer arg)
    // HIGH:  add full register state on syscall entry, all audit records

typedef enum { GATE_OK, GATE_DEGRADED, GATE_FAILED } gate_health_t;

gate_health_t gate_health_check(char* reason, size_t len);
    // Checks:
    //   - Syscall dispatcher table intact (all handlers present?)
    //   - Audit log not entirely overwritten (can still find recent records)
    //   - Rate limiter timers advancing
    //   - Circuit breaker counts reasonable (not all processes sandboxed)

err_t gate_fault_inject(gate_fault_type_t t);
    // FAULT_SYSCALL_RANDOM: random syscalls return ERR_NOSYS
    // FAULT_AUDIT_DROP:    audit records silently dropped
    // FAULT_RATE_LIMIT_ALL: every syscall rate-limited (1 token/sec)
    // FAULT_COPY_FAULT:    every copy_from_user fails

typedef struct {
    uint64_t syscall_count_total;
    uint64_t syscall_by_number[32];       // per-syscall count
    uint64_t syscall_errors;
    uint64_t syscall_denied_cap;
    uint64_t syscall_denied_rate;
    uint64_t circuit_breaker_trips;
    uint64_t signals_delivered;
    uint64_t audit_records_written;
    uint64_t audit_records_overwritten;
    float    avg_syscall_latency_us;
    float    p99_syscall_latency_us;
} gate_stats_t;

gate_stats_t gate_stats(void);
```

---

# 9. LAYER N: USER PROGRAM ENVIRONMENT

## 9A. Layer Specification

**Purpose**: Provide the runtime environment for user-space programs. This layer has zero kernel privileges and communicates with the kernel exclusively through the syscall interface.

**Responsibilities**:
- Process execution (init, shell, user daemons, applications)
- User-space standard library (libc) wrapping syscalls
- Dynamic linking and loading
- User-space heap management (`malloc`/`free` via `brk`/`mmap` syscalls)
- User-space threading (pthreads library built on `sys_thread_create`, `sys_mutex_lock`, etc.)

**Does NOT own**:
- Kernel memory, interrupts, page tables, file system internals, process table
- Any direct hardware access

## 9B. Internal Module Breakdown

| Module | Function |
|--------|----------|
| `init` | PID 1: mounts filesystems, starts essential daemons, launches shell |
| `libc` | C standard library: syscall wrappers, string functions, stdio, heap (`malloc`), threading (`pthread`) |
| `dynamic_linker` | Loads shared libraries into process address space, resolves relocations |
| `user_threading` | pthreads implementation: thread_create via `sys_thread_create`, mutex/condvar via syscalls |
| `cruntime` | C runtime: `_start` → calls `main()`, calls `exit()` on return |

## 9C. Data Structures

```c
// User-space only (not visible to kernel):
typedef struct heap_segment_t {
    void*    start;
    uint64_t size;
    bool     free;
    struct heap_segment_t* next;
    struct heap_segment_t* prev;
} heap_segment_t;

typedef struct user_mutex_t {
    volatile int locked;           // 0=free, 1=locked, 2=locked-with-waiters
    // This is the futex-like mechanism:
    //   - If CAS(0→1) succeeds, mutex acquired without syscall
    //   - If CAS(1→2) fails, thread calls sys_mutex_lock (kernel manages wait queue)
    uint32_t  padding;             // 8-byte aligned
} user_mutex_t;
```

## 9D. API Surface

(The syscall table from Layer N-1 is the full API.)

## 9E. Interaction Contract

**Expectations from Layer N-1**:
- Syscalls are reliable, safe, and return documented errors
- Kernel never corrupts user memory
- Signals are delivered asynchronously but safely

**Guarantees to nothing above** (top layer).

## 9F. Edge Cases and Fault Handling

| Scenario | Handling |
|----------|----------|
| **User program crash (SIGSEGV)** | Kernel delivers signal; if no handler, default action = terminate process; shell reports "Segmentation fault" |
| **malloc returns NULL** | User library should handle; typical behavior: return NULL to caller, program may abort |
| **Stack overflow (user)** | Guard page at bottom of user stack → access → SIGSEGV; `ulimit -s` controls stack size |
| **Infinite loop** | No kernel intervention (no timer dependency). User may Ctrl+C → shell sends SIGINT |
| **User modifies its own capability table** | Capability table is kernel-owned; user cannot access it directly; syscall layer validates all cap tokens |
| **User tries to execute privileged instruction** | CPU raises #GP (general protection fault); kernel delivers SIGILL |
| **Fork bomb** | Process table exhausted; spawn returns ERR_PROC_LIMIT; OOM killer may activate |

## 9G. Debuggability

(Rich enough libc functions: `printf`, `perror`, `strerror`.)

---

# 10. CROSS-CUTTING CONCERNS

## 10.1 Capability-Based Security Model

### Design
Every secured resource (file, device, IPC queue, process, memory region) is identified by a kernel object ID. Access is granted exclusively through capability tokens.

### Token Structure
```c
typedef struct cap_token_t {
    uint64_t    id;                // 64-bit random, kernel-CSPRNG generated
    uint64_t    object_id;         // what resource
    cap_type_t  type;              // FILE_READ | FILE_WRITE | FILE_EXEC | DEVICE_ACCESS
                                   // | IPC_SEND | IPC_RECV | PROCESS_KILL | PROCESS_DEBUG
                                   // | CAP_DELEGATE | MEM_MAP
    uint64_t    rights;            // bitmask of allowed operations
    uint64_t    expires_at_ns;     // 0 = permanent
    uint32_t    depth;             // 0 = root-issued, max 5
    bool        revoked;           // soft-delete
    uint64_t    owner_pid;         // process that owns this token
} cap_token_t;
```

### Issuance
- Process creation: `init` process gets all-root caps
- Delegation: `sys_cap_issue(parent_token, type, object_id, rights)` creates a derived token with depth+1
- Revocation: `sys_cap_revoke(token_id)` sets revoked=true on token and all derived tokens (recursive)
- Revocation notification: event bus publishes `CAP_REVOKED` → affected processes get signal

### Validation Path
```
syscall_entry → capability_validate(proc, token, required_type, required_rights)
    → lookup token in proc->cap_table
    → check token.revoked == false
    → check token.expires_at_ns == 0 || token.expires_at_ns > now
    → check token.type matches required_type
    → check (token.rights & required_rights) == required_rights
    → return PASS/FAIL
```

### Delegation Chain Limit
- Max depth = 5 (prevents unbounded delegation)
- Each delegation: depth = parent.depth + 1
- If depth >= 5: ERR_CAP_DEPTH

### Audit
- Every capability validation is logged (syscall audit trail includes cap token hash)
- Revocation events are WAL-logged before being applied

## 10.2 Unified Event Bus (Kernel-Internal)

### Design
A lightweight pub/sub message bus between Layers 1–5. Events are fixed-size structs (128 bytes). Subscriptions are registered at initialization or runtime.

### Event Types
```
PAGE_FAULT       { pid, vaddr, fault_flags }
PROCESS_DIED     { pid, exit_code }
PROCESS_CREATED  { pid, parent_pid, name }
DEVICE_READY     { major, minor }
DEVICE_FAILED    { major, minor, reason_code }
CAP_REVOKED      { pid, token_id, object_id }
OOM_KILL         { victim_pid, memory_freed }
FS_NEAR_FULL     { mount_point, free_pct }
TIMER_SKEW       { expected_ns, actual_ns }
WATCHDOG_EVENT   { layer_id, health_status, reason }
MODE_CHANGE      { old_mode, new_mode, trigger }
WAL_COMMIT       { sequence_no }
```

### API
```c
err_t eventbus_init(void);
err_t eventbus_publish(event_type_t type, event_data_t* data);
err_t eventbus_subscribe(event_type_t type, eventbus_callback_t cb, void* context);
err_t eventbus_unsubscribe(event_type_t type, eventbus_callback_t cb);
```

### Delivery Guarantees
- **Synchronous subscribers** (registered by layers): invoked in FIFO order during `eventbus_publish`
- **Asynchronous subscribers** (registered by watchdog): queued to a work thread; no delay to publisher
- Non-blocking: subscribers must not block; if a subscriber needs to block, it defers to a worker thread
- Event ordering: per-type ordering is preserved; cross-type ordering is not guaranteed

## 10.3 Layer Health Monitor (Watchdog Daemon)

### Design
A privileged kernel thread (runs at Layer 2 priority, but conceptually outside normal stack) that polls all layers at a configurable interval (default: 500ms).

### Polling Loop
```
loop:
  for each layer L in [1, 2, 3, 4, 5, N-1]:
    start = clock_get_monotonic_ns()
    health = L.layer_health_check()
    latency = clock_get_monotonic_ns() - start
    if health == FAILED or latency > 100ms:
      eventbus_publish(WATCHDOG_EVENT, { layer: L, status: health, reason: reason })
      if L.is_restartable():
        L.restart()    // reinitialize the layer module
      else:
        enter_safe_mode()
    if consecutive_failures[L] > 3:
      if L.is_critical():
        enter_emergency_mode()      // Layers 3-5 sealed, only 0-2 active
      else:
        L.restart()
    if mode == EMERGENCY:
      poll only Layers 0-2
  sleep(500ms)
```

### Layer Restart Protocol
1. Pause all operations in layer (drain queues, wait for in-flight ops to complete)
2. Save minimal state (or reconstruct from WAL)
3. Reinitialize layer module (call `_init` again)
4. Restore event bus subscriptions
5. Resume operations

## 10.4 Time Namespace and Monotonic Clock

### Design
Embedded in Layer 2 timekeeping module. Every layer uses `clock_get_monotonic_ns()` — no layer reads hardware timers directly.

### Architecture
```c
// Hardware timer (PIT/HPET) generates IRQ0 at HZ frequency
// Layer 2 accumulates: system_ticks++
// Monotonic time = system_ticks * (1,000,000,000 / HZ) + fractional_ns

// Per-process time namespace offset
struct time_namespace {
    int64_t  monotonic_offset_ns;   // applied to clock_gettime(CLOCK_MONOTONIC)
    int64_t  wall_offset_ns;        // applied to clock_gettime(CLOCK_REALTIME)
    bool     isolated;              // if true, time appears frozen (for containers)
};
```

### Properties
- Monotonic clock: always non-decreasing
- Wall clock: settable only by processes with `CLOCK_SET` capability
- Time namespaces: containers see shifted or frozen time

## 10.5 Write-Ahead Log (WAL) for Kernel State

### Design
A circular log on a reserved disk partition (or a reserved area of the boot partition). Every critical state mutation is recorded before the mutation is applied.

### WAL Record Types
```
WAL_PROCESS_CREATE  { pid, parent_pid, timestamp }
WAL_PROCESS_EXIT    { pid, exit_code }
WAL_MEM_ALLOC       { pid, vaddr, size_in_pages }
WAL_MEM_FREE        { pid, vaddr, size_in_pages }
WAL_INODE_MODIFY    { fs_id, inode_no, modified_fields_bitmap }
WAL_INODE_CREATE    { fs_id, inode_no, parent_inode, name }
WAL_INODE_DELETE    { fs_id, inode_no }
WAL_CAP_ISSUE       { pid, token_id, object_id, type, rights }
WAL_CAP_REVOKE      { token_id }
WAL_MODE_CHANGE     { old_mode, new_mode, trigger }
WAL_CHECKPOINT      { sequence_no }    // all prior records safely applied
```

### WAL API
```c
err_t wal_init(void);
err_t wal_append(wal_record_type_t type, wal_record_t* record);
err_t wal_flush(void);                  // force write to disk
err_t wal_recover(void);                // replay or rollback on boot
err_t wal_checkpoint(void);             // trim log up to sequence_no
```

### Recovery Protocol
1. On boot, scan WAL from last CHECKPOINT
2. For each record: check if mutation was applied (idempotency)
3. If mutation NOT applied: redo it
4. If mutation partially applied: either complete or roll back
5. After full replay, write new CHECKPOINT
6. If WAL itself is corrupt: attempt best-effort recovery, enter SAFE mode

## 10.6 Graceful Degradation Modes

### Mode Definitions

| Mode | Layers Active | Scheduling | Memory | File System | Process |
|------|--------------|------------|--------|-------------|---------|
| NORMAL | 0–5, N-1, N | Full | Full R/W | Full R/W | Create/manage |
| SAFE | 0–3, N-1 (limited) | Essential threads only | Read-mostly (no swap) | Read-only (root), write allowed on /tmp | No new processes |
| EMERGENCY | 0–2 | Only watchdog + recovery | No paging, kernel heap only | Sealed, read-only | No user processes. All killed. |

### Transition Triggers (→)

| From | To | Trigger |
|------|----|---------|
| NORMAL | SAFE | OOM unrecoverable, critical layer restart fails, watchdog detects unrecoverable fault |
| SAFE | NORMAL | Operator command (must have full capability), health check passes for 30s |
| NORMAL | EMERGENCY | Double fault, unrecoverable kernel heap corruption, watchdog detects Layer 2 failure |
| SAFE | EMERGENCY | Worsening condition: FS error propagates, more layers fail |
| EMERGENCY | NORMAL | Only via full reboot |

### Layer Behavior Per Mode

Each layer's APIs check `system_mode`:
- In SAFE mode: Layer 4 returns ERR_PERM for large allocations. Layer 5 returns ERR_PERM for non-root write operations. Layer 3 returns ERR_PERM for `process_spawn`.
- In EMERGENCY mode: Layer 3 and above are skipped entirely. Layer 1 and 2 are in survival mode.

---

# 11. BOOT SEQUENCE

## 11.1 Init Order (Bottom-Up)

```
Step  : Layer : Action
──────────────────────────────────────────────────────────────────
 1    | L0    | CPU powers on, starts in real mode
 2    | L1    | Bootloader (e.g., GRUB) loads kernel image into RAM
 3    | L1    | entry.s: switch to protected mode, then long mode
 4    | L1    | hal_init(boot_info):
      |       |   - Parse memory map from bootloader
      |       |   - Set up GDT, IDT (empty at first)
      |       |   - Initialize PIC (mask all IRQs)
      |       |   - Initialize UART (115200 baud)
      |       |   - Initialize physical frame allocator
      |       |   - Enable A20 gate
      |       |   - Transition to long mode (x86-64)
      |       |   - Auto-probe drivers on PCI bus
 5    | L2    | sched_init():
      |       |   - Initialize run queues (256 priority levels)
      |       |   - Set up idle thread (per CPU)
      |       |   - Create kernel thread for timer tick handler
      |       |   - Initialize IPC subsystem
      |       |   - Initialize timekeeping (read RTC, start monotonic)
      |       |   - Register IRQ0 handler (timer), IRQ1 handler (keyboard if present)
      |       |   - Enable interrupts (sti)
 6    | L3    | proc_init():
      |       |   - Initialize process table
      |       |   - Create init process (PID 1, kernel thread at this stage)
      |       |   - Initialize capability manager
      |       |   - Initialize sync primitives
 7    | L4    | vm_init(memory_size, boot_info):
      |       |   - Build buddy allocator over all free RAM
      |       |   - Create slab caches (thread_t, process_t, file_t, etc.)
      |       |   - Map kernel text/data at KERNEL_BASE (identity mapped + higher half)
      |       |   - Initialize OOM killer context
      |       |   - Initialize swap manager (probe swap partition)
      |       |   - Initialize page cache
 8    | L5    | vfs_init():
      |       |   - Initialize VFS (inode cache, dentry cache, mount table)
      |       |   - Mount root filesystem (detected from boot info)
      |       |   - Mount devfs on /dev
      |       |   - Mount tmpfs on /tmp
      |       |   - Mount procfs on /proc
      |       |   - Register block device for root device
      |       |   - Start write-back daemon
 9    | N-1   | syscall_gate_init():
      |       |   - Populate syscall dispatch table (32 entries)
      |       |   - Initialize audit log buffer
      |       |   - Initialize per-process rate limiter defaults
      |       |   - Install syscall handler (e.g., write to MSR_LSTAR for syscall/sysret)
      |       |   - Initialize shell (stdin/stdout/stderr = /dev/uart)
10    | L3    | init completes:
      |       |   - init process switches to user mode
      |       |   - Loads /sbin/init from root filesystem
      |       |   - init runs: mounts fstab entries, starts daemons
11    | N     | init spawns shell (on /dev/uart)
12    | --    | WATCHDOG DAEMON starts (kernel thread, Layer 2)
      |       |   - Begins polling all layers at 500ms interval
13    | --    | EVENTBUS initialized (connected by all layers)
14    | --    | WAL: initial checkpoint written (boot complete)
```

## 11.2 Dependency Graph

```
        L0 (Hardware)
            |
            v
        L1 (HAL) ←───────┐
            |              │
            v              │
        L2 (Sched/IPC) ───┤ (depends on L1 for timer, IRQ)
            |              │
            v              │
        L3 (Process) ─────┤ (depends on L2 for sched/ipc)
            |              │
            v              │
        L4 (Memory) ──────┤ (depends on L1 for phys alloc bootstrap, L3 for process_t)
            |              │
            v              │
        L5 (FS/VFS) ──────┤ (depends on L4 for page cache, L1 for block I/O)
            |              │
            v              │
        N-1 (Syscall) ────┤ (depends on L3, L4, L5)
            |              │
            v              │
         N (User) ────────┘
```

## 11.3 If a Layer Fails to Initialize

| Layer | Failure Handling |
|-------|-----------------|
| L1    | Cannot happen (no software init). If hardware is non-functional, triple-fault → reset. |
| L2    | If scheduler init fails: no multitasking → kernel panic (cannot recover). Print "Scheduler init failed" via UART, halt. |
| L3    | If process table init fails: kernel panic. If init process (PID 1) creation fails: kernel panic. |
| L4    | If buddy allocator init fails: no memory management → kernel panic. If swap init fails: continue without swap (logged warning). Kernel heap (slab) init failure: panic. |
| L5    | If root FS mount fails: kernel panic (no filesystem). If devfs mount fails: continue, but /dev absent. If tmpfs fails: continue without /tmp. If procfs fails: continue without /proc (diagnostic info lost). |
| N-1   | If syscall table setup fails: kernel panic (user cannot communicate). If audit init fails: continue without audit (logged). |
| N     | If init process (user-space) crashes: kernel panic with "No init. System halted." |
| Watchdog | If watchdog daemon fails to start: continue without watchdog (degraded state). |

---

# 12. SHUTDOWN SEQUENCE

## 12.1 Graceful Shutdown Order (Top-Down)

```
Step : Layer : Action
──────────────────────────────────────────────────────────────────
 1    | ----  | Trigger: poweroff, reboot, or Ctrl+Alt+Del
 2    | N     | init sends SIGTERM to all processes
      |       | Waits 5s for graceful exit, then SIGKILL
      |       | Process termination logged to WAL
 3    | N-1   | Disable syscall entry (MSR_LSTAR = 0)
      |       | Flush audit log to serial/FB (last messages)
      |       | Signal user: "System shutting down..."
 4    | L5    | vfs_sync(): flush all dirty pages to disk
      |       | Unmount all non-root filesystems
      |       | Remount root FS read-only
      |       | Flush block cache
 5    | L4    | Swap off: page in all swapped pages (or discard if shutting down)
      |       | Tear down page tables (keep kernel identity map)
      |       | Free all user pages
      |       | OOM killer disarmed
 6    | L3    | Kill all remaining threads
      |       | Clean up process table
      |       | Clean up capability tables
 7    | L2    | Disable interrupts (cli)
      |       | Stop scheduler (no context switches)
      |       | Stop timer
      |       | Drain IPC queues
 8    | L1    | wal_flush(): final WAL commit
      |       | hal_shutdown():
      |       |   - Notify ACPI for poweroff (if available)
      |       |   - Detach all drivers
      |       |   - Park CPU (hlt loop)
 9    | L0    | (hardware) poweroff or ACPI event
```

## 12.2 Crash Recovery

1. On next boot, bootloader loads kernel
2. `hal_init` → sets up basic hardware
3. `wal_init` → reads WAL from disk
4. `wal_recover` → replays or rolls back incomplete transactions
5. Normal boot sequence continues
6. If WAL recovery found inconsistencies: system boots in SAFE mode, operator must run fsck manually

## 12.3 Final WAL Commit

Before shutdown completes:
```c
wal_append(WAL_CHECKPOINT, { sequence_no: current_max });
wal_flush();  // ensure on disk
// After this, system may lose power safely — next boot will start from this checkpoint
```

---

# APPENDIX A: SMP EXTENSION SUMMARY

| Area | Uniprocessor | SMP Change |
|------|-------------|------------|
| **Layer 1** | Single PIC | per-CPU APIC + IOAPIC; IPI delivery; physical frame allocator per-CPU caches |
| **Layer 2** | Single run queue | per-CPU run queues; load balancing via push/pull migration; IPI for TLB shootdown and reschedule |
| **Layer 3** | Single process table lock | rwlock (readers: getpid, wait; writer: fork, exit); priority inheritance cross-CPU |
| **Layer 4** | Single buddy allocator | per-CPU page caches (hot pages); TLB shootdown on unmap; per-process page table lock |
| **Layer 5** | Single inode cache | RCU for dentry cache; per-bucket block cache locks |
| **N-1** | Single syscall entry | syscall entry on any CPU (syscall/sysret handles CPU migration transparently) |
| **Watchdog** | Single watchdog thread | One watchdog thread per CPU; each monitors local layers |
| **New races** | None | Two CPUs wake same thread, concurrent fork, TLB shootdown during page table switch, concurrent OOM kill |

# APPENDIX B: HARDWARE REQUIREMENTS SUMMARY

| Component | Minimum | Recommended | Notes |
|-----------|---------|-------------|-------|
| CPU | x86-64, 1 core | x86-64, 4+ cores | Long mode, paging, NX bit required |
| RAM | 512 MB | 2 GB+ | 512 MB fits kernel + minimal user space |
| Storage | 64 MB | 512 MB+ | Boot + root FS + swap partition |
| Serial | 16550 UART | Same | Primary debug console |
| Timer | PIT | HPET | HPET provides better resolution |
| Interrupt Ctrl | 8259A PIC | xAPIC | APIC required for SMP |

---

# 13. UNIVERSAL ERROR TAXONOMY

## 13.1 Design Rules

- `err_t` is a signed 32-bit integer: `0 = ERR_OK`. Negative = error. Positive = warning/partial success.
- Generic errors (ERR_GENERAL through ERR_BADFD at values -1 through -20) are defined in `types.h` and used by all layers.
- Layer-specific error codes extend the generic set at values -21 and below.
- Every error code has: numeric value, short name, description, defining layer, layers that may return it, and mandatory caller response.

## 13.2 Error Code Table

### Generic Errors (defined in types.h)

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| 0 | ERR_OK | Success | — |
| -1 | ERR_GENERAL | Unspecified failure | Propagate |
| -2 | ERR_NOMEM | Out of memory | Retry (with backoff) or propagate |
| -3 | ERR_INVAL | Invalid argument | Propagate |
| -4 | ERR_BADADDR | Bad address (NULL or unmapped) | Propagate |
| -5 | ERR_BUSY | Resource temporarily busy | Retry |
| -6 | ERR_TIMEOUT | Operation timed out | Retry or propagate |
| -7 | ERR_AGAIN | Try again (non-blocking) | Retry |
| -8 | ERR_FAULT | Page fault in user copy | Propagate |
| -9 | ERR_NOSYS | Function not implemented | Propagate |
| -10 | ERR_PERM | Permission denied | Propagate to caller |
| -11 | ERR_EXIST | Resource already exists | Propagate |
| -12 | ERR_NOENT | No such entry | Propagate |
| -13 | ERR_IO | I/O error | Retry (3×), then propagate |
| -14 | ERR_NOSPACE | No space left on device | Propagate |
| -15 | ERR_NAMETOOLONG | Name exceeds length limit | Propagate |
| -16 | ERR_LOOP | Symlink loop detected | Propagate |
| -17 | ERR_STALE | Stale file handle | Propagate |
| -18 | ERR_DEADLOCK | Deadlock detected | Propagate or panic |
| -19 | ERR_CAP | Capability denied | Propagate |
| -20 | ERR_BADFD | Bad file descriptor | Propagate |

### Generic Extensions

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -21 | ERR_NULL | NULL pointer passed | Panic (kernel) / propagate (user) |
| -22 | ERR_OVERFLOW | Buffer/counter overflow | Propagate |
| -23 | ERR_UNDERFLOW | Buffer/counter underflow | Propagate |
| -24 | ERR_CANCELED | Operation canceled | Propagate |
| -25 | ERR_STATE | Invalid state for operation | Propagate or panic |
| -26 | ERR_NOTFOUND | Resource not found | Propagate |

### Layer 1 (HAL) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -30 | ERR_IO_TIMEOUT | I/O operation timed out | Retry (3×), then propagate DEVICE_FAILED to event bus |
| -31 | ERR_IO_DMA | DMA transfer fault | Abort transfer, log error, notify driver |
| -32 | ERR_DRV_PROBE | Driver probe failed | Skip driver, notify device manager |
| -33 | ERR_DRV_MISSING | No driver for device | Log, return ERR_NOTFOUND to caller |
| -34 | ERR_DOUBLE_FREE | Physical page freed twice | Panic (kernel memory corruption) |
| -35 | ERR_IRQ_STORM | >10,000 IRQs/sec on one line | Mask IRQ, log, notify watchdog |

### Layer 2 (Scheduler / IPC / Interrupts) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -40 | ERR_SCHED_FULL | All priority queues full | Propagate (should never occur in practice) |
| -41 | ERR_IPC_QFULL | IPC destination queue full | Return ERR_AGAIN (non-blocking) or block sender |
| -42 | ERR_IPC_QEMPTY | IPC queue empty (non-blocking recv) | Return ERR_AGAIN |
| -43 | ERR_CLOCK_STALL | Timer tick hasn't advanced in 1s | Notify watchdog, enter SAFE mode |
| -44 | ERR_IRQ_STORM_GLOBAL | Total IRQ rate exceeds 50,000/sec | Throttle all non-critical IRQs |
| -45 | ERR_IPC_CAP | Missing capability for IPC operation | Return ERR_PERM, log audit |

### Layer 3 (Process / Thread / Sync) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -50 | ERR_PROC_LIMIT | Process table full | Return ERR_NOMEM to fork |
| -51 | ERR_THREAD_LIMIT | Per-process thread limit exceeded | Return ERR_NOMEM |
| -52 | ERR_CAP_DEPTH | Capability delegation chain too deep | Return ERR_PERM |
| -53 | ERR_BROKEN_MUTEX | Mutex owner thread is dead | Panic (kernel lock corruption) |
| -54 | ERR_ALREADY_ZOMBIE | wait() on already-reaped child | Return ERR_NOENT |

### Layer 4 (VMM / PMM) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -60 | ERR_OOM | Out of memory (physical + swap exhausted) | Invoke OOM killer; return ERR_NOMEM |
| -61 | ERR_SWAP_FULL | Swap partition exhausted | Degrade to non-swap mode; propagate |
| -62 | ERR_SWAP_IO | Swap I/O error | Degrade: disable swapping for this page; propagate |
| -63 | ERR_TLB_SHOOTDOWN | TLB shootdown IPI timed out | Retry IPI (3×), then panic |
| -64 | ERR_PF_KERNEL | Page fault in kernel mode | Panic with full register dump |
| -65 | ERR_SLAB_CORRUPT | Slab allocator detected corruption | Panic (heap corruption) |
| -66 | ERR_PML4_FULL | No free PML4 entries | Return ERR_NOMEM |

### Layer 5 (VFS / Block / FS) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -70 | ERR_DISK_FULL | Storage full | Return ERR_NOSPACE |
| -71 | ERR_FS_CORRUPT | Filesystem metadata corruption | Remount read-only; propagate |
| -72 | ERR_PATH_LONG | Path exceeds PATH_MAX | Return ERR_NAMETOOLONG |
| -73 | ERR_SYMLINK_LOOP | Too many symlink resolutions | Return ERR_LOOP |
| -74 | ERR_STALE_HANDLE | File handle refers to deleted inode | Return ERR_STALE |
| -75 | ERR_BUSY_UNMOUNT | Device busy, cannot unmount | Return ERR_BUSY |

### Layer N-1 (Syscall) Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -80 | ERR_BAD_SYSCALL | Unknown syscall number | Return -ERR_INVAL to user |
| -81 | ERR_BAD_FD | Invalid file descriptor | Return -ERR_BADFD to user |
| -82 | ERR_RATE_LIMITED | Syscall rate limit exceeded | Sleep and retry (user-level) |
| -83 | ERR_CIRCUIT_OPEN | Circuit breaker open for this syscall | Block until circuit resets |
| -84 | ERR_COPY_FROM_USER | copy_from_user faulted | Return -ERR_FAULT to user (kills process if malicious) |
| -85 | ERR_COPY_TO_USER | copy_to_user faulted | Return -ERR_FAULT to user |

### Cross-Cutting Errors

| Value | Name | Description | Caller Response |
|-------|------|-------------|-----------------|
| -90 | ERR_WATCHDOG_EXPIRED | Layer health check failed repeatedly | Escalate degradation mode |
| -91 | ERR_WAL_FULL | WAL ring buffer full | Block until consumer drains |
| -92 | ERR_EVENT_OVERFLOW | Event bus ring full | Drop event, log overflow counter |

## 13.3 Error Propagation Contract

**Rule:** When layer L_N receives an error from L_{N-1}, it MUST do one of:

1. **Propagate as-is** — for errors that are universally meaningful (ERR_INVAL, ERR_NOMEM, ERR_PERM, ERR_NOTFOUND). The error code is returned unchanged to the next layer up.
2. **Wrap with layer prefix** — for errors that gain meaning from context. The layer returns its own error code but preserves the original as a WAL record. Example: VFS gets ERR_IO from block layer, wraps it as ERR_FS_CORRUPT.
3. **Handle internally** — for transient errors where retry makes sense. Example: scheduler gets ERR_AGAIN from IPC (queue full), blocks the sender and retries. The caller never sees the error.

**Per-error category:**

| Error Category | Rule | Rationale |
|----------------|------|-----------|
| ERR_INVAL, ERR_BADADDR, ERR_NULL | Propagate as-is | Always means the caller passed bad arguments |
| ERR_NOMEM, ERR_OOM | Propagate as-is | Critical; upper layer must decide mitigation |
| ERR_PERM, ERR_CAP_DEPTH | Propagate as-is | Security-critical; must reach caller |
| ERR_TIMEOUT, ERR_AGAIN, ERR_BUSY | Layer retries internally (3×) | Transient conditions that may clear |
| ERR_IO_TIMEOUT, ERR_DRV_PROBE | Wrap with layer error | Driver state change; upper layer gets ERR_IO or device notification |
| ERR_SLAB_CORRUPT, ERR_DOUBLE_FREE | Panic | Memory corruption is unrecoverable |
| ERR_DEADLOCK, ERR_BROKEN_MUTEX | Panic | Sync primitive corruption is unrecoverable |
| ERR_PF_KERNEL, ERR_SWAP_IO | Panic or SAFE mode | Depends on context; kernel page fault = panic, swap I/O = SAFE mode |
| ERR_IRQ_STORM, ERR_CLOCK_STALL | Handle (mask IRQ / notify watchdog) | The layer mitigates without burdening upper layers |
| ERR_RATE_LIMITED, ERR_CIRCUIT_OPEN | Propagate as-is | The syscall layer stamps these explicitly |

---

# 14. KERNEL PANIC HANDLER

## 14.1 Design

`panic()` is the last line of defense — called when no recovery is possible. It must work even with corrupt heap, near-full stack, dead scheduler, or partially broken UART.

### Invariants
- `panic()` is callable from any layer, any context (interrupt, thread, NMI handler)
- On entry: immediately disables interrupts (`cli`) and acquires a spinlock (`panic_lock`)
- If `panic_lock` is already held (re-entrant panic): halt immediately with no further output
- After panic: system is stopped. No resumption is attempted.

### Panic Output Protocol

The panic handler prints to serial in strict order. Each step is attempted; if the UART is broken, `outb` to the UART port is a no-op and the CPU continues to the next step:

1. `\n\n*** KERNEL PANIC ***\n`
2. Panic reason string (the `fmt` + args passed to `panic()`)
3. Faulting instruction pointer and stack trace:
   - Read RBP chain up to 16 frames
   - For each frame, print RIP and (if symbol table present) symbol name
   - If RBP chain is broken, attempt to scan for valid return addresses
4. Full register dump:
   - RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP
   - R8, R9, R10, R11, R12, R13, R14, R15
   - RIP, RFLAGS, CS, SS
   - CR0, CR2, CR3, CR4
5. Last 32 WAL records from the in-memory WAL ring buffer (do not attempt disk I/O)
6. Last 64 audit log records
7. Per-layer health snapshot — call each layer's `health_check` function with a 100ms timeout. If a check hangs, skip that layer and log "TIMEOUT".
8. Memory summary — free pages, total pages, swap usage, top 5 consumers by PID
9. Watchdog status — last known state of each monitored layer

### Panic Modes

| Mode | Behavior | Trigger |
|------|----------|---------|
| `PANIC_HALT` | Print full dump, then `cli; hlt` loop | Default; unspecified errors |
| `PANIC_REBOOT` | Same output, then write crash cookie to reserved RAM, then reboot (via `outb(0x64, 0xFE)`) | Configurable via kconfig |
| `PANIC_DUMP` | Same output, then write full physical RAM to swap partition (overwriting swap), then halt | Configurable for post-mortem debugging |

### Nested Panic Protection

```c
static volatile int panicking = 0;

void panic(const char* fmt, ...) {
    asm volatile("cli");
    if (__sync_lock_test_and_set(&panicking, 1)) {
        // Already panicking — re-entrant. Halt immediately.
        for (;;) asm volatile("cli; hlt");
    }
    // ... output ...
    // Modes:
    //   PANIC_HALT:  for (;;) asm volatile("cli; hlt");
    //   PANIC_REBOOT: write crash cookie, outb(0x64, 0xFE)
    //   PANIC_DUMP:   copy RAM to swap, then halt
}
```

### SMP Panic Protocol

- The CPU that hits `panic()` sends an NMI IPI to all other CPUs via the APIC.
- Each receiving CPU saves its register state to a per-CPU panic buffer (`panic_cpu_state[cpu_id]`), then halts.
- The primary CPU includes a summary of secondary CPU states in its output.

### Crash Cookie

A 512-byte structure at a fixed physical address (e.g., `0x0009_FC00`, in the BIOS data area low region, reserved in the E820 map).

```c
typedef struct {
    uint32_t magic;           // 0x505A4E43 ("PNZC" — Panic Cookie)
    uint32_t version;         // Crash cookie format version
    uint64_t timestamp;       // Monotonic ticks at panic
    uint64_t fault_rip;       // Instruction pointer that panicked
    uint64_t cr2;             // CR2 at time of panic
    uint32_t panic_mode;      // PANIC_HALT / PANIC_REBOOT / PANIC_DUMP
    uint32_t cpu_count;       // Number of CPUs that saved state
    char reason[128];         // Panic reason string (truncated)
    uint8_t reserved[352];    // Zeroed; reserved for future expansion
    uint32_t crc32;           // CRC32 of bytes 0–508
} crash_cookie_t;
```

The cookie is written before any reboot attempt. On next boot, the boot sequence checks for a valid cookie; if found, it prints "*** CRASH DETECTED ON PREVIOUS BOOT ***" and displays the cookie contents before clearing it.

### Debuggability Interface

```c
trace_t panic_trace(int level);                   // Last N panic traces
health_t panic_check_health(void);                // Panic subsystem health
void panic_fault_inject(uint32_t type);            // Force panic (for testing)
panic_stats_t panic_get_stats(void);               // Panic count, last reason
```

---

# 15. VIRTUAL ADDRESS SPACE LAYOUT

## 15.1 Complete Map (x86-64, 48-bit Canonical)

```
0x0000_0000_0000_0000 ─────────────────────────────────────────────────────
                        NULL GUARD PAGE (unmapped, 4KB)
                        Catches NULL pointer dereference at virtual 0.
0x0000_0000_0000_1000 ─────────────────────────────────────────────────────
                        USER CODE (text, RX)
                        Entry point, program code.
0x0000_0000_0040_0000 ─────────────────────────────────────────────────────
                        USER DATA / BSS (RW, NX)
0x0000_0000_0080_0000 ─────────────────────────────────────────────────────
                        USER HEAP (RW, NX, grows upward)
                        Managed via brk() / sbrk(). Start = heap_base.
                        Heap limit = heap_base + heap_max (configurable per process).
0x0000_0000_4000_0000 ─────────────────────────────────────────────────────
                        MEMORY-MAPPED REGIONS (mmap, RW/NX or RWX)
                        File-backed and anonymous mappings.
0x0000_0000_8000_0000 ─────────────────────────────────────────────────────
                        USER STACK (RW, NX, grows downward, 8MB max)
                        Guard page below stack bottom.
0x0000_0000_FFFF_FFFF ─────────────────────────────────────────────────────
                        vDSO PAGE (kernel-mapped, user-readable + executable, 4KB)
                        Fast syscall trampoline: clock_gettime, getpid.
                        Structure: vdso_data_t shared, updated by kernel each timer tick.
0x0000_0001_0000_0000 ─────────────────────────────────────────────────────
                        TLS AREA (Thread-Local Storage, RW, NX)
                        Per-thread via arch_prctl(ARCH_SET_FS).
0x0000_0001_0000_1000 ─────────────────────────────────────────────────────
                        SIGNAL TRAMPOLINE PAGE (kernel-mapped, user-executable, 4KB)
                        sigreturn trampoline. Read-only to user, executable.
0x0000_7FFF_FFFF_FFFF ─────────────────────────────────────────────────────
                        ─── END OF USER SPACE ───
══════════════════════════════════════════════════════════════════════════
                        NON-CANONICAL GAP
                        (Access always triggers #GP)
══════════════════════════════════════════════════════════════════════════
0xFFFF_8000_0000_0000 ─────────────────────────────────────────────────────
                        KERNEL TEXT (RX)
                        Code, read-only, executable.
0xFFFF_8000_0020_0000 ─────────────────────────────────────────────────────
                        KERNEL RODATA (R, NX)
                        Const tables, syscall dispatch table, symbol table.
0xFFFF_8000_0040_0000 ─────────────────────────────────────────────────────
                        KERNEL DATA / BSS (RW, NX)
                        Global variables, static buffers.
0xFFFF_8000_0080_0000 ─────────────────────────────────────────────────────
                        KERNEL HEAP (slab allocator pool, RW, NX)
                        All `kmalloc` / `kfree` allocations come from here.
0xFFFF_8000_0800_0000 ─────────────────────────────────────────────────────
                        PHYSICAL MEMORY DIRECT MAP (RW, NX)
                        All 512MB RAM identity-mapped here.
                        phys_to_virt(pa) = pa + DIRECT_MAP_BASE
                        virt_to_phys(va) = va - DIRECT_MAP_BASE
                        For easy access to any physical address from kernel code.
0xFFFF_8000_2800_0000 ─────────────────────────────────────────────────────
                        PER-CPU DATA AREAS (RW, NX)
                        One 64KB region per CPU (max 64 CPU = 4MB total).
                        Accessed via `mov fs:0, ...` with FS base set per CPU.
0xFFFF_8000_2840_0000 ─────────────────────────────────────────────────────
                        KERNEL STACKS (RW, NX)
                        Each thread: 16KB kernel stack + 4KB guard page below.
                        Guard page triggers #PF on stack overflow.
                        8192 stacks × 20KB = 160MB maximum.
0xFFFF_8000_3800_0000 ─────────────────────────────────────────────────────
                        MMIO WINDOW (device registers, RW, NX or UC)
                        PCI MMIO BARs mapped here. Up to 512MB.
0xFFFF_8000_5800_0000 ─────────────────────────────────────────────────────
                        WAL BUFFER (in-memory ring, RW, NX)
                        16MB ring buffer for Write-Ahead Log records.
                        Flushed to disk asynchronously.
0xFFFF_8000_5900_0000 ─────────────────────────────────────────────────────
                        AUDIT LOG BUFFER (in-memory ring, RW, NX)
                        4MB ring buffer for audit events.
0xFFFF_8000_5940_0000 ─────────────────────────────────────────────────────
                        WATCHDOG SCRATCH AREA (RW, NX)
                        Per-layer health state, counters.
0xFFFF_8000_5950_0000 ─────────────────────────────────────────────────────
                        CRASH COOKIE (RW, NX, 512 bytes)
                        Survives soft reboot. Written on panic + reboot.
0xFFFF_FFFF_FFFF_FFFF ─────────────────────────────────────────────────────
```

## 15.2 Region Ownership & Access Rules

| Region | Owner | Appears In | Permissions | Unauthorized Access |
|--------|-------|------------|-------------|---------------------|
| User text | Process (Layer 3) | User PT | RX | — |
| User data | Process (Layer 3) | User PT | RW, NX | — |
| User heap | Process (Layer 3) | User PT | RW, NX | — |
| mmap regions | Process (Layer 3) | User PT | Per-mapping | — |
| User stack | Process (Layer 3) | User PT | RW, NX | — |
| vDSO | Kernel (Layer N-1) | User PT | R+X | — |
| TLS | Process (Layer 3) | User PT | RW, NX | — |
| Signal trampoline | Kernel (Layer N-1) | User PT | R+X | — |
| Kernel text | Kernel (Layer 4) | Kernel PT | RX | #PF → panic |
| Kernel rodata | Kernel (Layer 4) | Kernel PT | R, NX | #PF → panic |
| Kernel data | Kernel (Layer 4) | Kernel PT | RW, NX | #PF → panic |
| Kernel heap | Kernel (Layer 4) | Kernel PT | RW, NX | #PF → panic |
| Direct map | Kernel (Layer 4) | Kernel PT | RW, NX | #PF → panic |
| Per-CPU areas | Kernel (Layer 2) | Kernel PT | RW, NX | #GP on invalid FS base |
| Kernel stacks | Kernel (Layer 2) | Kernel PT | RW, NX | Guard page #PF |
| MMIO | Kernel (Layer 1) | Kernel PT | UC+ | Platform-defined |
| WAL buffer | Cross-cutting | Kernel PT | RW, NX | — |
| Audit buffer | Cross-cutting | Kernel PT | RW, NX | — |
| Watchdog scratch | Cross-cutting | Kernel PT | RW, NX | — |
| Crash cookie | Cross-cutting | Kernel PT | RW, NX | — |

## 15.3 Kernel/User Page Table Split

**Design choice: Shared page table with SMEP/SMAP/UMIP (no KPTI by default).**

Rationale: KPTI (Kernel Page Table Isolation) adds measurable overhead (~5–15%) to every syscall and interrupt. For a uniprocessor teaching/experimental kernel, the Meltdown mitigation is not justified. SMEP (Supervisor Mode Execution Prevention) and SMAP (Supervisor Mode Access Prevention) are used instead: kernel code cannot execute user pages, and kernel data accesses to user memory must use explicit `copy_from_user`/`copy_to_user` wrappers.

- **User page tables** contain: all user regions + kernel text (RX) + kernel rodata (R) + vDSO + signal trampoline
- **Kernel page tables** contain: all kernel regions + all user regions (but SMAP prevents unintended access)
- Context switch reloads CR3 to the next process's PML4 (which shares kernel entries via PML4[511])

## 15.4 Stack Layout at Syscall Entry

When `syscall` instruction executes (user → kernel):

```
High address
┌─────────────────────────────┐
│         User stack          │  ← RSP before syscall (user RSP)
├─────────────────────────────┤
│         [gap]               │
├─────────────────────────────┤
│         Saved user RSP      │  ← Pushed by kernel entry stub (mov from user RSP)
│         Saved user RIP      │  ← From LSTAR MSR (or return address)
│         Saved RFLAGS        │  ← From SYSCALL (clears IF, TF, AC)
│         SS, RSP             │  ← From kernel stack after swap
│         [alignment gap]     │
├─── syscall_frame_t ─────────┤
│         RAX (syscall num)   │
│         RCX                 │
│         RDX                 │
│         RSI                 │
│         RDI                 │
│         R8                  │
│         R9                  │
│         R10                 │
│         R11                 │
│         [saved callee regs] │
└─────────────────────────────┘
Low address (RSP after entry)
```

## 15.5 Page Fault Address Routing

Given faulting address CR2:

| CR2 Range | Handler | Action |
|-----------|---------|--------|
| User space (0x0..0x0000_7FFF_FFFF_FFFF) | Layer 4 (VMM) VMA-based lookup | If VMA exists: demand-page, COW, or page-in. If no VMA: SIGSEGV to process |
| 0x0..0x1000 (null page) | Layer 4 (VMM) | SIGSEGV (NULL dereference) — no VMA present |
| Kernel text/rodata (0xFFFF_8000_0000_0000..0xFFFF_8000_0040_0000) | Kernel panic | Write to read-only kernel memory |
| Kernel heap/data (0xFFFF_8000_0040_0000..0xFFFF_8000_0800_0000) | Kernel panic | Wild pointer in kernel |
| Direct map (0xFFFF_8000_0800_0000+) | Kernel panic | Corrupted physical address access |
| MMIO window | Layer 1 (HAL) | Consult driver MMIO map; if unmapped → panic |
| Kernel stack guard page | Kernel panic | Stack overflow detected |
| Non-canonical | #GP (not #PF) | CPU rejects non-canonical address before paging |

---

# 16. SCHEDULER POLICY SPECIFICATION

## 16.1 Time Slice Assignment

| Priority Band | Range | Slice (ms) | Preemptible By |
|---------------|-------|------------|----------------|
| IDLE | 0 | ∞ (only runs when nothing else ready) | Any non-idle thread |
| BACKGROUND | 1–63 | 10 ms | Any higher priority thread |
| NORMAL | 64–127 | 8 ms | Interactive, high, realtime |
| INTERACTIVE | 128–191 | 4 ms | High, realtime |
| HIGH | 192–254 | 2 ms | Realtime only |
| REALTIME | 255 | 1 ms | Only equal-priority (FIFO within band) |

**Voluntary yield:** When a thread calls `thread_yield()` before its slice expires, it goes to the back of its priority queue and receives a **full slice** when it next runs. This prevents the "yield starvation" problem where yielding repeatedly consumes only partial slices.

## 16.2 Starvation Prevention (Aging)

- Every 200ms, the scheduler scans the run queues.
- For each thread that has not run for more than 2 seconds, its **effective priority** is incremented by 1 step per 200ms of starvation.
- **Cap**: A thread can age up to a maximum effective priority of 192 (bottom of HIGH band). This ensures no thread can age itself into the REALTIME band.
- **Reset**: After a thread runs for one full slice, its effective priority = base priority.
- Aging is applied only to bands IDLE through INTERACTIVE. HIGH and REALTIME threads do not age (to preserve priority consistency).

## 16.3 Preemption Points

### Preemption allowed:
- Return from syscall handler (before returning to user mode)
- Return from interrupt handler (before iretq)
- Explicit calls to `thread_yield()`, `schedule()`, `sched_block()`, `sched_wake()`
- Any call to `pmm_alloc_page()` or `pmm_free_page()` if the allocator needs to reschedule (future SMP balancing)

### Preemption suppressed:
| Context | Tracking | Max Duration |
|---------|----------|-------------|
| Spinlock held | `current_thread->preempt_depth` | ≤10 µs (no I/O inside spinlock) |
| Interrupt handler | `in_interrupt` per-CPU flag | ≤50 µs |
| Scheduler critical section (run queue manipulation) | `sched_lock` held | ≤5 µs |
| Timer tick ISR (watchdog handler) | ISR context | ≤20 µs |

```c
// API:
void preempt_disable(void);   // increment preempt_depth on current thread
void preempt_enable(void);    // decrement; if 0 and need_resched, calls schedule()
int  preempt_is_disabled(void); // return preempt_depth > 0
```

Nesting: `preempt_disable` / `preempt_enable` are ref-counted. Preemption actually re-enables only when the depth returns to 0.

## 16.4 Idle Thread

```c
void idle_thread(void* arg) {
    for (;;) {
        if (check_sleepers()) // wake any timed-out sleepers
            schedule();
        asm volatile("sti; hlt; cli"); // C1 halt, wakes on any interrupt
    }
}
```

- The idle thread runs at priority 0 and is always ready.
- It enters ACPI C1 (HLT) when no thread needs to run. Any interrupt (timer tick, device IRQ) wakes it.
- Future: For C2/C3 integration, see Section 22 (Power Management).
- The idle thread also performs deferred garbage collection: free zombie threads, coalesce freed slabs, compact page tables.

## 16.5 Realtime Guarantees

| Metric | Bound | Dependencies |
|--------|-------|-------------|
| Worst-case interrupt latency | ≤5 µs | From IRQ assertion to first ISR instruction. Depends on: longest non-preemptible section (≤5 µs spinlock) + hardware IRQ delivery latency |
| Worst-case scheduling latency (PRIO 255) | ≤10 µs (IRQ path) / ≤50 µs (syscall path) | From `sched_wake()` to first instruction. Depends on: pending IRQ handling + scheduler run queue lock acquisition |
| Maximum delay due to spinlocks | 10 µs | Spinlock critical sections must not contain I/O or blocking operations |
| Timer tick jitter | ≤100 µs | PIT-based; switch to HPET reduces to ≤10 µs |

---

# 17. SYSCALL ABI (APPLICATION BINARY INTERFACE)

## 17.1 Invocation Mechanism

**Primary:** `syscall` instruction (MSR_LSTAR). Fast path, used for all standard syscalls.
**Legacy fallback:** `int 0x80`. Provided for compatibility with older toolchains. The int 0x80 handler uses the same dispatch table but a slower entry path (saves more registers, checks more conditions).

The kernel allocates syscall numbers as follows:

| Range | Purpose |
|-------|---------|
| 0–31 | Process control (exit, fork, exec, wait, getpid, etc.) |
| 32–63 | File I/O (open, close, read, write, lseek, ioctl, mmap, munmap, etc.) |
| 64–95 | IPC (send, recv, create_queue, destroy_queue, etc.) |
| 96–127 | Scheduling (sched_yield, sched_get_prio, sched_set_prio, nanosleep, etc.) |
| 128–159 | Synchronization (mutex_lock, mutex_unlock, cond_wait, cond_signal, etc.) |
| 160–191 | Capability (cap_issue, cap_revoke, cap_delegate, cap_probe, etc.) |
| 192–223 | Debug/audit (trace_enable, audit_read, etc.) |
| 224–255 | Architecture-specific (arch_prctl, cache_flush, etc.) |

## 17.2 Register Assignment

### On syscall entry (user → kernel via `syscall`):

| Register | Role |
|----------|------|
| RAX | Syscall number |
| RDI | Argument 1 |
| RSI | Argument 2 |
| RDX | Argument 3 |
| R10 | Argument 4 (syscall uses R10 instead of RCX because `syscall` clobbers RCX with RIP) |
| R8 | Argument 5 |
| R9 | Argument 6 |

### Return from kernel:

| Register | Role |
|----------|------|
| RAX | Return value (positive/zero = success; negative = -error_code) |

### Register preservation:

| Register | Preserved? |
|----------|-----------|
| RBX | Preserved (callee-saved) |
| R12–R15 | Preserved (callee-saved) |
| RBP | Preserved (callee-saved) |
| RSP | Preserved (user RSP restored on return) |
| RDI, RSI, RDX, RCX, R8, R9, R10, R11 | Clobbered (caller-saved) |

### RFLAGS handling:

| Bit | Behavior |
|-----|----------|
| IF | Cleared by `syscall`; restored by `sysret` |
| TF | Cleared by `syscall` |
| AC | Cleared by `syscall` |
| DF | Preserved |
| All other user-set bits | Preserved |

### Error Return Convention

- Positive or zero return value in RAX → success.
- Negative return value in RAX → error (`-err_code`).
- The libc stub translates this: if RAX < 0, store `-RAX` into `errno` and return -1; otherwise return RAX.

### Restartable Syscalls

- **Interruptible syscalls** (read, write, wait, ipc_recv, mutex_lock, cond_wait): if a signal arrives while the syscall is blocked, it returns `-ERR_INTR` (-14) to user space.
- **Automatic restart:** If the signal handler was installed with `SA_RESTART`, the kernel adjusts the saved RIP in the signal frame to point back to the `syscall` instruction rather than after it. On return from signal handler, the syscall re-executes.
- The syscall dispatch distinguishes restartable vs. non-restartable syscalls via a flags bitmask in the syscall table.

### vDSO

The vDSO is a single 4KB page mapped into user space. It contains:

```c
typedef struct {
    uint32_t version;             // vDSO format version
    uint32_t tsc_khz;            // TSC frequency in kHz (calibrated at boot)
    volatile uint64_t monotonic_ns; // Updated by kernel each timer tick
    volatile uint64_t wall_clock_ns; // Updated by kernel each timer tick
    uint64_t last_tsc;           // TSC value at last update
    uint32_t multiplier;         // For TSC → ns conversion
    uint32_t shift;              // Shift count for fixed-point math
    uint8_t reserved[4012];      // Padding to 4096 bytes
} vdso_data_t;
```

User programs call `clock_gettime(CLOCK_MONOTONIC, &ts)` via vDSO:
1. Read `monotonic_ns` from the shared page (atomic load).
2. No syscall needed — result is always current (updated every timer tick).

If the vDSO page is not mapped (e.g., static binary), the libc falls back to a real `clock_gettime` syscall.

---

# 18. DRIVER FRAMEWORK DEEP SPECIFICATION

## 18.1 Driver Types

| Type | Description | Examples |
|------|-------------|---------|
| Character | Byte-stream device; implements `open`, `close`, `read`, `write`, `ioctl` | UART, keyboard, mouse |
| Block | Sector-based device; implements `open`, `close`, `read_blocks`, `write_blocks`, `ioctl` | ATA, AHCI, NVMe, virtio-blk |
| Bus | Enumerates child devices on a bus; implements `probe`, `remove`, `scan` | PCI, USB, ACPI |
| Virtual | No hardware backing; provides synthetic device nodes | /dev/null, /dev/zero, /dev/random |
| Filter | Wraps another driver; transforms data passing through it | Encryption layer, compression layer, RAID |

## 18.2 Driver Version Compatibility

```c
#define DRIVER_API_VERSION  0x0100  // Major.Minor, 1.0

typedef struct {
    uint16_t api_version;           // Required DRIVER_API_VERSION
    uint16_t driver_version;        // Driver-specific version
    char     name[32];              // Driver name
    // ...
} driver_header_t;
```

- **Exact match** (api_version == DRIVER_API_VERSION): driver loads normally.
- **Minor mismatch** (api_version major == DRIVER_API_VERSION major, minor differs): kernel attempts to load with a **compatibility shim** that translates between minor versions. If a shim is not available, the driver is rejected.
- **Major mismatch**: driver is rejected with ERR_DRV_PROBE.

## 18.3 Driver Dependency Graph

Drivers declare dependencies at compile time:

```c
driver_dep_t deps[] = {
    { "pci_bus", DEP_MANDATORY },
    { "timer",   DEP_OPTIONAL },
    { NULL, 0 }
};
```

- At boot: the driver manager resolves dependencies topologically. A driver is only probed after all its MANDATORY dependencies have successfully probed.
- If a MANDATORY dependency fails to probe, the dependent driver is marked FAILED without even attempting to probe.
- If an OPTIONAL dependency fails, the dependent driver loads but its health check notes the missing dependency.
- **Cascade rule**: If driver A depends on driver B, and B fails at runtime (e.g., device removed), A is immediately notified via event bus (`DEVICE_FAILED` with B's ID) and enters a DEGRADED state.

## 18.4 Driver Failure Isolation

Each driver receives a `driver_sandbox_t`:

```c
typedef struct {
    void*        mmio_base;       // MMIO window mapped to this driver
    size_t       mmio_size;
    uint64_t     dma_mask;        // DMA addressable range
    uint64_t     allowed_pages_mask; // MPK protection key mask
    spinlock_t   lock;            // Driver lock
} driver_sandbox_t;
```

- Drivers run in kernel space but with MPK (Memory Protection Keys, if available) limiting their writeable pages. A driver can only write to its own data structures and its MMIO window.
- If a driver panics or corrupts memory: the kernel detects the violation (page fault in driver-owned region) and:
  1. Marks the driver as FAILED.
  2. Removes its device node from devfs.
  3. Notifies Layer 1 (HAL) to release the device.
  4. Publishes `DEVICE_FAILED` on the event bus.
  5. Service threads using the device receive ERR_IO on their next operation.

## 18.5 Driver I/O Model

**DMA descriptors (scatter-gather list):**

```c
typedef struct {
    uint64_t phys_addr;        // Physical address of buffer
    uint32_t length;           // Length in bytes
    uint32_t flags;            // Next bit, direction, etc.
} dma_segment_t;

typedef struct {
    dma_segment_t segments[16]; // Max 16 segments per DMA descriptor
    uint32_t      count;        // Number of valid segments
    uint32_t      total_bytes;  // Total transfer length
    void*         callback;     // Completion callback (may be NULL)
} dma_descriptor_t;
```

**Interrupt-driven vs. polling:**

- Default: interrupt-driven (IRQ registered via `hal_irq_register`).
- The driver exposes a threshold: if IRQ rate exceeds `irq_storm_threshold`, the driver temporarily switches to polling mode (every 100µs check device status register) for 10ms, then re-enables interrupts.
- Switch back to interrupt mode if the polling loop finds no pending work 3 consecutive times.

**MMIO flush requirements:**

- Before reading from MMIO after writing: `asm volatile("mfence" ::: "memory")` to ensure write is visible to device.
- For WC (write-combining) memory: `asm volatile("sfence" ::: "memory")` after last write.

### Debuggability Interface

```c
trace_t  driver_trace(driver_t* drv, int level);
health_t driver_check_health(driver_t* drv);
void     driver_fault_inject(driver_t* drv, uint32_t fault_type);
driver_stats_t driver_get_stats(driver_t* drv); // IRQ count, bytes transferred, errors
```

---

# 19. IPC SECURITY & THREAT MODEL

## 19.1 Data Structures

```c
typedef struct {
    ipc_queue_id_t queue_id;      // Kernel-assigned, unique
    pid_t          owner_pid;      // Creating process
    cap_token_t    send_cap;       // Capability required to send
    cap_token_t    recv_cap;       // Capability required to receive
    int            max_messages;   // Queue depth
    int            cur_messages;
    ipc_message_t* ring;           // Ring buffer
    wait_queue_t   send_waiters;   // Blocked senders (queue full)
    wait_queue_t   recv_waiters;   // Blocked receivers (queue empty)
    spinlock_t     lock;
    uint32_t       per_sender_rate[64]; // Per-sender rate counters (hash by PID)
} ipc_queue_t;

typedef struct {
    ipc_queue_t*  send_queue;
    ipc_queue_t*  recv_queue;
    cap_token_t   channel_cap;    // Combined capability for the channel
} ipc_channel_t;
```

## 19.2 Threat Mitigation

### Threat 1: Unauthorized queue access
- **Attack:** Process without `IPC_SEND` capability tries to send to a queue.
- **Detection:** `ipc_send()` checks `cap_validate(send_cap, current->pid)` on entry.
- **Mitigation:** Returns `ERR_PERM`. An audit log entry records sender PID, target queue ID, and timestamp.

### Threat 2: Message spoofing
- **Attack:** Process A sends a message to process C claiming to be from process B.
- **Mitigation:** The kernel always stamps `sender_pid` in `ipc_message_t.header.sender_pid`. User code cannot set this field — `copy_from_user` into the header overwrites it with the true PID. Receiver verifies by checking `header.sender_pid`.

### Threat 3: Denial of service via queue flooding
- **Attack:** Attacker floods a queue with messages, starving legitimate senders.
- **Mitigation (per-sender rate limit):**
  - Each queue maintains a hash table of sender PIDs and their message counts over a sliding 100ms window.
  - If one sender exceeds 50% of queue capacity within 100ms, subsequent messages from that sender are rejected with `ERR_RATE_LIMITED` until the window resets.
- **Mitigation (queue admission control):**
  - No single sender may occupy more than 50% of the queue's message slots simultaneously.
  - `ipc_send` checks: `sender_count[hash(pid)] < max_messages / 2`.

### Threat 4: Uninitialized message payload
- **Attack:** A sender leaves kernel data in the message buffer (uninitialized memory).
- **Mitigation:** `kmemset(message, 0, sizeof(ipc_message_t))` before handing the receive buffer to the receiver. Message slots are zeroed on queue creation and on every `ipc_recv` completion before the slot is recycled.

### Threat 5: Capability laundering via IPC
- **Attack:** Process A encodes a capability token in an IPC message payload. Process B extracts it.
- **Mitigation:** `ipc_message_t` has an explicit `cap_fields` array:
  ```c
  typedef struct {
      uint32_t cap_count;           // Number of caps in this message
      cap_token_t caps[8];          // Explicitly declared caps
      uint8_t payload[480];         // Opaque data (no caps allowed here)
  } ipc_message_t;
  ```
  - The kernel inspects `cap_count` on send. For each cap in `caps[]`, it validates that the sender holds the cap AND the receiver has `CAP_RECEIVE_CAP` right. If both satisfied, the cap token is duplicated for the receiver (with depth incremented by 1).
  - Caps smuggled in the `payload` field are not detected but also cannot be used because `cap_validate` requires the token to be in the sending process's token list. The receiver cannot import a raw token without kernel mediation.

### Threat 6: TOCTOU on capabilities
- **Attack:** Process checks cap validity, cap is revoked, process acts on it.
- **Mitigation:** At the start of any operation requiring a capability, the kernel:
  1. Validates the capability (`cap_validate`).
  2. Acquires a **use-right** (a short-lived lease that prevents revocation for the duration of the operation).
  3. Performs the operation.
  4. Releases the use-right.
  - Revocation of the capability marks the token as invalid but does not affect already-acquired use-rights. Use-rights have a maximum lifetime of 10ms (enforced by a timestamp). If the kernel detects a stale use-right (expired but not released), it panics (indicating a kernel bug).

## 19.3 IPC Channel Model

An IPC channel is a kernel-managed name-service abstraction over a queue pair:

**Creation:**
```c
err_t ipc_channel_create(pid_t local_pid, const char* name, 
                         ipc_channel_t** channel);
```
- Registers `name` in a kernel-wide namespace (flat, no hierarchy, max 64 chars).
- Returns a channel with two queues: one for A→B, one for B→A.
- Sender/Receiver capabilities are automatically generated and stored in the channel.

**Discovery:**
```c
err_t ipc_channel_open(const char* name, cap_token_t capability, 
                       ipc_channel_t** channel);
```
- Caller must hold the capability to the channel (obtained via pre-arranged shared secret or from `init` process during boot).
- `ipc_channel_open` validates the capability, then returns a reference to the existing channel.

**Rendezvous (synchronous) IPC:**
- For small messages ≤ 512 bytes, IPC can be rendezvous-style:
  1. `ipc_send(channel, msg, RENDEZVOUS)` blocks the sender.
  2. Kernel copies the message directly into the receiver's address space when `ipc_recv(channel)` is called.
  3. Transfer is zero-copy for the payload (uses shared memory established at channel creation).
  4. Both sender and receiver unblock simultaneously.

---

# 20. NETWORK STACK PLACEHOLDER & INTEGRATION POINTS

## 20.1 Layer Placement

**Chosen placement: Layer 5.5, between Layer 5 (VFS) and Layer N-1 (Syscall).**

Rationale: The network stack needs access to VFS for socket file descriptors (which map to VFS operations) and block devices for storage, but it also exports its own file operations (send/recv = special write/read). Placing it at 5.5 allows it to use VFS abstractions while not tightly coupled to any specific filesystem.

## 20.2 Socket File Descriptor Integration

Sockets are implemented as a special file type in VFS:

```c
file_ops_t socket_file_ops = {
    .read   = socket_read,    // calls recv
    .write  = socket_write,   // calls send
    .close  = socket_close,
    .ioctl  = socket_ioctl,   // setsockopt, getsockopt
    .poll   = socket_poll,    // select/poll support
    .mmap   = NULL,           // not supported
};
```

Syscalls `socket`, `bind`, `connect`, `listen`, `accept` are implemented in Layer 5.5, but they create a VFS file descriptor (`fd = allocate_fd()`) and associate it with a socket structure. The socket operations then go through the standard `read`/`write` dispatch in VFS.

## 20.3 Network Capabilities

| Capability | Operation |
|-----------|-----------|
| `CAP_NET_BIND` | Bind to a port < 1024 (privileged ports) |
| `CAP_NET_CONNECT` | Make outbound connections |
| `CAP_NET_ADMIN` | Modify routing table, firewall rules, set promiscuous mode |
| `CAP_NET_RAW` | Open raw sockets (SOCK_RAW, requires CAP_NET_ADMIN) |

Without `CAP_NET_BIND`, a process may only bind to ports ≥ 1024.

## 20.4 NIC Driver Integration (Layer 1)

A NIC driver implements:

```c
err_t hal_net_send_frame(netbuf_t* buf);           // Transmit a raw frame
err_t hal_net_recv_frame(netbuf_t** buf);           // Receive a raw frame (blocking or non-blocking)
int   hal_net_link_status(void);                    // 1 = link up, 0 = link down
err_t hal_net_set_mac(uint8_t mac[6]);              // Set MAC address
err_t hal_net_set_promisc(int enabled);             // Enable/disable promiscuous mode
```

The NIC driver is registered with the network stack during boot. The network stack maintains a list of available NICs.

## 20.5 Packet Buffer Allocator

Network buffers need their own allocator separate from the page cache because:
- Packets arrive in interrupt context (cannot sleep while allocating)
- Packets have variable sizes (64–1518 bytes for Ethernet, up to 9000 for jumbo frames)
- Packet buffers must be DMA-aligned (typically 512-byte aligned for NIC descriptors)

```c
typedef struct {
    uint8_t* data;              // Pointer to payload start within the buffer
    uint32_t len;               // Payload length
    uint32_t headroom;          // Space before data (for protocol headers)
    uint32_t tailroom;          // Space after data + len
    uint32_t total_size;        // Total buffer size (typically 2KB per buffer)
    uint64_t phys_addr;         // Physical address (for DMA)
    uint8_t  refcount;          // Shared buffer tracking
    uint8_t  flags;             // Allocated from IRQ pool? DMA-safe?
} netbuf_t;
```

**Allocation pools:**
- **IRQ-safe pool:** Pre-allocated ring of 64 netbufs of size 2KB each. Used in interrupt context for received frames. Never sleeps.
- **Normal pool:** Standard `kmalloc`-based. Used in process context for outgoing frames. May sleep.

## 20.6 Event Bus Integration

| Event | Emitter | Consumers | Payload |
|-------|---------|-----------|---------|
| `NET_PACKET_READY` | NIC driver (Layer 1) | Network stack (Layer 5.5) | NIC ID, netbuf pointer |
| `NET_LINK_UP` | NIC driver (Layer 1) | Network stack, syslog | NIC ID, speed |
| `NET_LINK_DOWN` | NIC driver (Layer 1) | Network stack, syslog | NIC ID, reason |
| `NET_ADDR_CHANGE` | Network stack | syslog, firewall | NIC ID, new IP |

## 20.7 Loopback Device

The `lo` device is a trivial driver that immediately delivers sent frames back as received:

```c
err_t lo_send(netbuf_t* buf) {
    netbuf_t* recv_buf = netbuf_clone(buf);  // Clone the buffer
    netif_rx(lo_device, recv_buf);            // Push to receive path
    netbuf_free(buf);
    return ERR_OK;
}
```

---

# 21. SYSTEM-WIDE TEST & VERIFICATION FRAMEWORK

## 21.1 Test Categories

| Category | Scope | Environment | Frequency |
|----------|-------|-------------|-----------|
| Unit | Single layer (others mocked) | Host (Linux) | Every commit |
| Integration | 2+ real layers, mocked hardware | QEMU | Every commit |
| Stress | Full OS under load | QEMU + real hardware | Nightly |
| Fault injection | Systematic fault injection | QEMU | Nightly |
| Regression | Every documented edge case | QEMU | Every commit |

## 21.2 Test Harness Architecture

```c
typedef enum {
    TEST_PASS,
    TEST_FAIL,
    TEST_PANIC    // Kernel panicked during test
} test_result_t;

typedef struct {
    const char* name;
    test_result_t (*setup)(void);          // Prepare test environment
    test_result_t (*run)(void);            // Execute test
    void          (*teardown)(void);       // Clean up
    const char*   expected;                // Expected result description
    uint32_t      timeout_ms;              // Test timeout
} test_case_t;
```

**Test runner flow:**
1. Discover all registered test cases (via linker-set array: `__start_test_cases` to `__stop_test_cases`).
2. For each test case:
   a. Call `setup()`. If it fails → mark TEST_FAIL, skip test.
   b. Call `run()`. If it panics → QEMU detects triple fault, test harness records TEST_PANIC.
   c. After `run()` returns (or timeout), call `teardown()`.
   d. Run **canary checks** (see below). If any fail → mark TEST_FAIL (state corruption).
3. Print summary: `PASS: N, FAIL: M, PANIC: P`.

**Canary mechanism (post-test state invariants):**
```c
bool test_canaries(void) {
    bool ok = true;
    if (pmm_free_pages_count() + pmm_allocated_pages_count() != pmm_total_pages())
        ok = false; // Page leak
    for (int i = 0; i < MAX_MUTEX; i++) {
        if (mutex_table[i].owner && !thread_is_alive(mutex_table[i].owner))
            ok = false; // Broken mutex (owner dead)
    }
    // ... additional invariants
    return ok;
}
```

**Output protocol:**
- Primary: serial console (`kprintf` for each test result).
- Secondary: a `/proc/test_results` pseudo-file that the test runner reads from the host via QEMU's serial pass-through + a `{BEGIN_RESULTS}` / `{END_RESULTS}` delimiter.

## 21.3 Critical Test Cases

### TC1: OOM Recovery
- **Setup:** Allocate pages until OOM.
- **Stimulus:** One more allocation request.
- **Expected:** OOM killer fires, selects a victim (PID != current), frees pages, allocation succeeds or returns ERR_NOMEM gracefully. No panic.

### TC2: Fork Bomb
- **Setup:** Spawn 10,000 processes (simulated via thread_create with max_procs limit).
- **Stimulus:** Try to spawn 10,001st process.
- **Expected:** Process limit (defined per system) is enforced. Return ERR_PROC_LIMIT. System remains responsive (shell accepts input within 100ms).

### TC3: Scheduler Starvation
- **Setup:** Create 255 priority-1 threads that spin-loop. Create 1 priority-128 thread.
- **Stimulus:** Start all threads.
- **Expected:** The priority-128 thread runs at least once every 5 seconds (aging mechanism ensures the priority-1 threads eventually boost to 128 within ~4s of starvation + 200ms of aging window).

### TC4: WAL Crash Recovery
- **Setup:** Open a WAL-logged filesystem. Write 100 files with known content (metadata recorded in WAL).
- **Stimulus:** Simulate power loss: QEMU `-no-shutdown` + kill QEMU process. Reboot. Mount FS.
- **Expected:** WAL replay completes. All 100 files exist with correct content. No metadata corruption.

### TC5: Capability Revocation
- **Setup:** Issue capability token A. Delegate to B (derived token). Delegate B to C.
- **Stimulus:** Revoke A.
- **Expected:** `cap_validate(A)` returns ERR_PERM. `cap_validate(B)` returns ERR_PERM. `cap_validate(C)` returns ERR_PERM. Watchdog logs no anomalies.

### TC6: Syscall Fuzzing
- **Setup:** Use a fuzz generator (offline, scripted).
- **Stimulus:** Send 1,000,000 random syscall numbers + argument combinations via QEMU serial.
- **Expected:** Zero kernel panics. Zero triple faults. `dmesg` contains no corruption warnings.

### TC7: Page Fault on Swapped Page
- **Setup:** Allocate memory, get physical page. Page it out to swap (force eviction).
- **Stimulus:** Access the virtual address.
- **Expected:** Page fault is handled. Page is paged in from swap. Correct data is returned.

### TC8: IPC Queue Flooding
- **Setup:** Create IPC queue, sender and receiver processes.
- **Stimulus:** Sender floods queue at maximum rate (spin-loop send).
- **Expected:** After 100ms, rate limiter engages. Receiver is not starved (receives at least 10% of messages). Queue does not overflow.

### TC9: Nested Interrupt Handling
- **Setup:** Configure two timers at different priorities (or simulate via fault injection).
- **Stimulus:** Trigger IRQ0, within handler trigger IRQ1 (nested).
- **Expected:** Both ISRs complete successfully. No stack overflow (verify by checking RSP delta before/after nesting). System continues normally.

### TC10: Graceful Degradation — Block Device Failure
- **Setup:** Mount a filesystem from a block device.
- **Stimulus:** Inject `DEVICE_FAILED` for the block device (fault injection or driver unload).
- **Expected:** VFS enters read-only mode. Syscalls to the failed device return `ERR_IO`. System continues in SAFE mode. Shell works for non-disk operations.

---

# 22. POWER MANAGEMENT

## 22.1 CPU Idle States (ACPI C-States)

| State | Instruction | Wakeup Latency | Power Saving | Wakeup Sources |
|-------|-------------|----------------|-------------|----------------|
| C0 | — | — | None | — |
| C1 | HLT | ~1µs | Minimal | Any interrupt |
| C2 | MWAIT (with hint) | ~100µs | Moderate | Any interrupt + bus master |
| C3 | MWAIT (with flush) | ~1ms | Significant | Only platform devices (timer, IPI, NIC) |

**Idle thread decision algorithm:**
```c
acpi_cstate_t idle_select_cstate(void) {
    uint64_t next_wakeup = sched_next_wakeup_ticks(); // Earliest timeout or thread wake
    uint64_t now = hal_timer_get_ticks();
    uint64_t idle_duration = next_wakeup - now;       // In timer ticks (1ms resolution)

    if (idle_duration < 2)     return C1;  // <2ms → fast halt
    if (idle_duration < 50)    return C2;  // 2–50ms → moderate sleep
    return C3;                              // >50ms → deep sleep
}
```

**C2/C3 wakeup guarantee:** Before entering C2 or C3, the kernel programs the PIT/HPET to fire at `next_wakeup` time. The timer interrupt (IRQ0) is a valid wakeup source for all C-states. After C2/C3 wakeup, the timer ISR runs normally.

## 22.2 CPU Frequency Scaling (ACPI P-States)

```c
typedef enum {
    CPUFREQ_PERFORMANCE,   // Max frequency always
    CPUFREQ_POWERSAVE,     // Min frequency always
    CPUFREQ_ONDEMAND       // Dynamic based on utilization
} cpufreq_governor_t;

typedef struct {
    uint32_t min_freq_mhz;
    uint32_t max_freq_mhz;
    uint32_t current_freq_mhz;
    cpufreq_governor_t governor;
    uint64_t last_sample_ticks;
    uint32_t busy_ticks_since_sample;
} cpufreq_policy_t;
```

**ONDEMAND algorithm (runs every 100ms):**
```
utilization = busy_ticks_since_sample / 100ms
if utilization > 0.80 → scale_up()
if utilization < 0.20 → scale_down()
busy_ticks_since_sample = 0
```

**P-state and TSC:** On P-state change, TSC frequency changes (on older CPUs). The kernel recalibrates TSC frequency by reading the HPET or ACPI PM timer immediately after the change, updating `vdso_data_t.multiplier` and `vdso_data_t.shift`.

## 22.3 System Sleep States

### S3 (Suspend-to-RAM) Sequence:
1. **Initiate:** `echo mem > /sys/power/state` or ACPI sleep button event.
2. **Layers save state:**
   - Layer 5: Flush all dirty pages to disk. Sync all filesystems.
   - Layer 4: Flush TLB, save page tables, mark all memory as self-refresh.
   - Layer 3: Send SIGSTOP to all processes (except kernel threads).
   - Layer 2: Stop scheduler. Save APIC state. Save IRQ controller state.
   - Layer 1: Save CPU registers to reserved RAM region (the "resume vector" area).
3. **ACPI:** Write S3 sleep type to PM1a_CNT register. CPU enters S3.
4. **Wakeup:** On wake (RTC alarm, power button, NIC magic packet), CPU resumes at the BIOS/UEFI-reserved wakeup vector. BIOS restores minimal state and jumps to the kernel's resume entry point.

### S3 Resume Sequence:
1. Kernel entry detects resume mode (checks a "resume magic" in the saved-state area).
2. Restore CPU registers from saved state.
3. Restore APIC, IRQ controller.
4. Restore page tables.
5. Reschedule scheduler.
6. Resume processes (they continue from where they stopped).
7. **Clock resync:** Read RTC, compute the wall-clock delta, adjust monotonic clock offset (monotonic clock must NOT go backward — the offset is increased by the sleep duration).

### S5 (Soft Off):
1. Flush WAL.
2. Sync all filesystems.
3. Power off: `outw(0xB004, 0x2000)` (QEMU) or ACPI `PM1a_CNT.SLP_TYP` = S5.

## 22.4 Timekeeping After Sleep

```c
// Called on resume from S3
void timekeeping_resume(void) {
    uint64_t rtc_epoch = rtc_read_epoch_seconds();     // Current wall clock
    uint64_t pre_sleep_epoch = saved_rtc_epoch;        // Saved before suspend
    uint64_t sleep_duration_ns = (rtc_epoch - pre_sleep_epoch) * 1000000000ULL;

    // Monotonic clock: adjust offset so it doesn't go backward
    monotonic_offset_ns += sleep_duration_ns;

    // Wall clock: use RTC value directly
    wall_clock_epoch_ns = rtc_epoch * 1000000000ULL;
}
```

## 22.5 Power Events on the Event Bus

| Event | Emitter | Consumers | Action |
|-------|---------|-----------|--------|
| `POWER_SUSPEND_REQUEST` | ACPI driver | Layer 2 (scheduler), Layer 5 (VFS) | Stop scheduling, sync FS, prepare for sleep |
| `POWER_RESUME` | Resume path | All layers | Resync clocks, restore drivers |
| `CPU_FREQ_CHANGE` | Cpufreq governor | Layer 2 (timekeeping) | Recalibrate TSC multiplier |
| `BATTERY_LOW` | ACPI battery driver | Watchdog, syslog | Log warning, reduce brightness, prepare for suspend |
| `AC_PLUG` | ACPI AC driver | Cpufreq governor | Switch to PERFORMANCE |
| `AC_UNPLUG` | ACPI AC driver | Cpufreq governor | Switch to POWERSAVE |

---

*End of Architecture Requirements Document*
