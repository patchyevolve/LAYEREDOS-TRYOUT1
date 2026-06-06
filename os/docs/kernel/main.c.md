# main.c — Kernel Entry Point

**Path:** `os/src/kernel/main.c`  
**Layer:** Boot orchestration (above all layers, orchestrates init sequence)

---

## Purpose

`kmain` is the C entry point called by `boot.S` after long mode is active
and the kernel stack is valid.  Its sole job is to initialise every
subsystem in the correct dependency order and then hand control to the shell.
It never returns.

---

## Xen PVH detection

```c
#define XEN_HVM_START_MAGIC 0x33687578
if (magic == XEN_HVM_START_MAGIC)
    mb_info = 0;
```

When booted via Xen's PVH mechanism, `magic` contains a Xen-specific value
instead of the multiboot magic `0x2BADB002`.  The multiboot info pointer
is meaningless in that case, so it is zeroed.  `hal_get_mem_size(0)` then
falls back to assuming 512 MB of RAM.

---

## Boot sequence

```c
void kmain(uint64_t magic, uint64_t mb_info)
```

The `BOOT_TOTAL_STEPS` macro is set to 6; each major step calls
`boot_report(desc)` which prints `[BOOT] Step N/6: desc`.

| Step | Call | Depends on |
|------|------|-----------|
| Pre-step | `hal_get_mem_size(mb_info)` | Must happen before hal_init so PMM knows the memory size |
| 1 | `hal_init(mb_info_phys)` | Nothing — first C subsystem |
| 2 | `pmm_init(mem_size, mb_info_phys)` | hal_init (uses `hal_get_mmap_entries`) |
| 3 | `vmm_init()` | pmm_init (needs page allocation) |
| 4 | `kmalloc_init()` | pmm_init (needs page allocation) |
| 5 | `syscall_init()` | hal_init (IDT already set up) |
| 6 | `sched_init()` | pmm_init + hal_init (allocates thread stacks, uses timer) |
| Post | `eventbus_init()` + `watchdog_init()` | sched_init (watchdog calls sched functions) |
| Post | `hal_irq_register(0, watchdog_timer_handler, NULL)` | watchdog_init |
| Post | `hal_uart_rx_init()` | hal_init |
| Post | `shell_init()` | All of the above |

---

## Why `hal_get_mem_size` before `hal_init`?

`hal_get_mem_size` parses the multiboot memory map from `mb_info` — a
physical address valid under the current identity mapping.  It must be
called while `mb_info` is still a live physical address; after full
initialisation, the multiboot data might be overwritten by PMM allocations.
The result is cached inside `hal.c` and returned later via
`hal_get_mmap_entries()`.

`hal_init` itself also calls `hal_get_mem_size` if needed, but `kmain`
calls it first so the PMM receives the size before `hal_init` prints
anything.

---

## `boot_report`

```c
static void boot_report(const char* desc) {
    boot_step++;
    kprintf("[BOOT] Step %d/%d: %s\n", boot_step, BOOT_TOTAL_STEPS, desc);
}
```

Pure diagnostic output.  `BOOT_TOTAL_STEPS = 6` is a compile-time constant;
if steps are added/removed it must be updated manually.

---

## After initialisation

```c
hal_sti();         // enable interrupts — timer ISR will now fire
shell_run();       // blocking: never returns
for (;;) asm volatile("cli; hlt");  // unreachable safety net
```

`hal_sti()` is the moment the system becomes live — timer ticks start,
the watchdog starts running, the scheduler's `need_reschedule` mechanism
becomes active.  `shell_run` is the first code that runs with interrupts
enabled.

---

## Static variable

```c
static uint64_t mb_info_phys = 0;
```

Saved at the top of `kmain` so it could be accessed by later code if needed
(e.g., a command to re-parse the multiboot map).  Currently only used as
a local within `kmain`.
