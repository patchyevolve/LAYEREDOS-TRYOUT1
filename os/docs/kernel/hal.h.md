# hal.h — HAL Public Interface

**Path:** `os/src/kernel/hal.h`  
**Layer:** Layer 1 (HAL) — header

---

## Purpose

Declares every type and function that layers above Layer 1 are allowed to
call.  Including this header is the only legitimate way to call into the
HAL from the scheduler, VMM, PMM, or shell.

---

## `int_frame_t` — interrupt register frame

```c
typedef struct int_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} int_frame_t;
```

This struct exactly mirrors the stack layout created by `isr_common_handler`
in `isr.S`.  A pointer to the top of this on-stack frame is passed to
`interrupt_handler` and (via it) to `syscall_handler`.

Fields are in push order — the assembler pushes `%r15` first (lowest
address after the vector/error pair) and `%rax` last, so `rax` is at the
lowest address in the struct.

---

## User-mode segment selectors

```c
#define USER_CS  0x1B   // selector 3 | RPL 3 — 64-bit user code
#define USER_DS  0x23   // selector 4 | RPL 3 — user data/stack
```

Used by `thread_create_user` when building the `iretq` frame, and by
`gdt_init` (implicitly via the GDT slot assignments).

---

## `irq_handler_t`

```c
typedef void (*irq_handler_t)(int_frame_t* frame, void* data);
```

Function pointer type for hardware IRQ handlers.  Registered with
`hal_irq_register`.  The `data` pointer is the opaque context passed
back to the handler (e.g., a device struct pointer).

---

## Interrupt control

```c
void hal_sti(void);
void hal_cli(void);
cpu_flags_t hal_save_irq(void);       // pushfq + cli, returns RFLAGS
void        hal_restore_irq(cpu_flags_t flags);  // sti if IF was set
```

`hal_save_irq` / `hal_restore_irq` are the correct way to enter a critical
section.  They preserve the previous interrupt-enable state so nested
critical sections work correctly (if interrupts were already disabled before
entering, they stay disabled when leaving).

---

## `isr_vectors[256]`

```c
extern uint64_t isr_vectors[256];
```

Table of ISR stub addresses defined in `isr.S`.  `idt_init()` iterates
this to install gates.  Entries for undefined vectors are zero (NULL).

---

## Memory map

```c
#define MAX_MMAP_ENTRIES 32
typedef struct { uint64_t start, end; uint32_t type; } hal_mmap_entry_t;
int hal_get_mmap_entries(hal_mmap_entry_t* out, int max);
uint64_t hal_get_mem_size(uint64_t mb_info);
```

`hal_get_mem_size` must be called **before** `hal_init` because `main.c`
calls it first to size the PMM bitmap.  It parses the multiboot info and
caches entries internally; `hal_get_mmap_entries` reads that cache.

---

## Multiboot structures

`mmap_entry_t` and `multiboot_info_t` are declared here (not in a separate
multiboot header) to avoid duplicating these structures across files.  The
`type == 1` value means "usable RAM" in the multiboot memory map.

---

## Full function reference

| Function | Description |
|----------|-------------|
| `hal_init(mb_info_phys)` | Full hardware init — GDT, IDT, PIC, UART, PIT |
| `hal_irq_register(irq, handler, data)` | Register an IRQ handler + unmask |
| `hal_irq_eoi(irq)` | Send End-Of-Interrupt to PIC |
| `hal_set_kernel_stack(rsp0)` | Update TSS.RSP0 for ring-3 → ring-0 transitions |
| `hal_timer_init(hz)` | Reprogram PIT frequency |
| `hal_timer_get_ticks()` | Monotonic tick counter |
| `hal_timer_get_ns()` | Ticks converted to nanoseconds (128-bit safe) |
| `hal_timer_get_hz()` | Current PIT frequency |
| `hal_uart_getchar()` | Read one character (blocking) |
| `hal_uart_data_available()` | Non-blocking check for pending RX data |
| `hal_uart_rx_init()` | Enable IRQ-driven UART receive |
| `hal_poweroff()` | ACPI/QEMU shutdown |
| `hal_reboot()` | Keyboard-controller reset |
| `hal_cpu_has_mwait()` | CPUID leaf 1 ECX.MONITOR bit — for idle optimisation |
