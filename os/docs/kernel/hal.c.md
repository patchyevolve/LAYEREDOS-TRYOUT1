# hal.c — Hardware Abstraction Layer

**Path:** `os/src/kernel/hal.c`  
**Layer:** Layer 1 (HAL)

---

## Purpose

Initialises and abstracts every piece of hardware the kernel depends on:
CPU descriptor tables (GDT, IDT, TSS), the Programmable Interrupt Controller
(PIC), the UART serial port, the Programmable Interval Timer (PIT), and
the IRQ dispatch table.  After `hal_init` returns, all hardware is ready
and the rest of the kernel never touches hardware registers directly.

---

## Hardware devices managed

| Device | I/O ports | Purpose |
|--------|-----------|---------|
| UART 16550 (COM1) | `0x3F8`–`0x3FD` | Serial console I/O |
| PIC 8259A master | `0x20`–`0x21` | IRQ routing, masking, EOI |
| PIC 8259A slave | `0xA0`–`0xA1` | IRQs 8–15 |
| PIT 8254 | `0x40`, `0x43` | Periodic timer at 1000 Hz |

---

## Global descriptor table (GDT)

Built by `gdt_init()` with 7 entries (indices 0–6):

| Index | Selector | Type | DPL | Notes |
|-------|----------|------|-----|-------|
| 0 | — | Null | — | Required by x86 |
| 1 | `0x08` | Code64 | 0 | Kernel code segment (`CS` after boot) |
| 2 | `0x10` | Data | 0 | Kernel data/stack (`DS`, `SS`) |
| 3 | `0x18` | Code64 | 3 | User code segment (`USER_CS = 0x1B` with RPL=3) |
| 4 | `0x20` | Data | 3 | User data/stack (`USER_DS = 0x23` with RPL=3) |
| 5–6 | `0x28` | TSS64 | 0 | 64-bit TSS descriptor (takes two slots) |

The TSS is loaded via `ltr $0x28`.  The selector for a TSS descriptor has
RPL=0 and TI=0 (GDT).  `0x28` = index 5 × 8 = 40 = `0x28`.

**User selectors:**  
`USER_CS = 0x1B` = (index 3 × 8) | RPL=3 = `0x18 | 0x3`.  
`USER_DS = 0x23` = (index 4 × 8) | RPL=3 = `0x20 | 0x3`.  
These match the `iretq` frame built by `thread_create_user`.

---

## Interrupt descriptor table (IDT)

256 entries, initialised by `idt_init()`.  The loop walks `isr_vectors[]`
(exported by `isr.S`) and calls `idt_set_gate(i, addr, dpl)` for every
non-NULL entry.

`idt_set_gate` fills an `idt_entry_t`:

| Field | Value | Meaning |
|-------|-------|---------|
| `selector` | `0x08` | Kernel code segment |
| `ist` | 1 if `vec==8`, else 0 | IST stack for double fault only |
| `type_attr` | `0x8E \| (dpl<<5)` | Present, 64-bit interrupt gate, given DPL |
| `offset_*` | ISR address split into 3 parts | Where to jump on interrupt |

DPL is 3 for vector 128 (`int $0x80`) so user code can invoke it.
All other vectors have DPL=0 — user code cannot manually trigger them.

---

## Task State Segment (TSS)

The TSS stores the kernel stack pointer that the CPU automatically loads
when a privilege-level transition (ring 3 → ring 0) occurs:

```c
tss.rsp[0]  = (uint64_t)user_stack0 + sizeof(user_stack0);
// = top of the 16 KB user_stack0 buffer in kernel .bss
```

When user code executes `int $0x80`, the CPU:
1. Sees DPL mismatch (ring 3 → ring 0 gate).
2. Reads `TSS.RSP0` and switches RSP to that value.
3. Pushes SS, old RSP, RFLAGS, CS, RIP onto the new stack.
4. Jumps to the IDT handler.

`hal_set_kernel_stack(uint64_t rsp0)` updates `tss.rsp[0]` when the
scheduler switches to a new thread, so each thread uses the top of its
own kernel stack as RSP0.

**IST stack:**
```c
tss.ist[0] = tss.ist[1] = (uint64_t)ist_stack0 + sizeof(ist_stack0);
```
IST 1 is used by the double-fault handler (IDT entry 8 has `ist=1`).
This guarantees the #DF handler always has a valid stack even if RSP was
corrupt when the fault was triggered.

---

## PIC remapping

The default PIC IRQ vectors (0–15) overlap the CPU exception vectors
(0–31).  `pic_remap()` remaps:
- Master PIC IRQs 0–7 → vectors 32–39
- Slave PIC IRQs 8–15 → vectors 40–47

After remapping the mask registers are set:
- Master: `0xFB` = mask all except IRQ2 (cascade) and IRQ0 (timer)
- Slave: `0xFF` = mask all

Individual IRQs are unmasked when a handler is registered via
`hal_irq_register`.

---

## IRQ registration

```c
err_t hal_irq_register(uint8_t irq, irq_handler_t handler, void* data);
```

- Stores handler + data in `irq_handlers[irq]`.
- Issues `__sync_synchronize()` (full memory barrier) to prevent the ISR
  from seeing a torn write.
- Unmasking is done automatically by clearing the relevant PIC mask bit.

The `interrupt_handler` dispatcher (called from `isr_common_handler`)
calls `irq_handlers[vec-32].handler(frame, data)` for hardware IRQs
(vectors 32–47).

---

## Timer

`hal_timer_init(1000)` programs the PIT to fire at 1000 Hz:

```
divisor = 1193182 / 1000 = 1193
Mode byte: 0x36 = channel 0, lobyte/hibyte, mode 3 (square wave)
```

`timer_ticks` is incremented in `interrupt_handler` on every vector-32
interrupt.  `hal_timer_get_ns()` converts ticks to nanoseconds using
128-bit arithmetic to avoid overflow:

```c
unsigned __int128 ns = (unsigned __int128)timer_ticks * 1000000000ULL;
return (uint64_t)(ns / timer_hz);
```

---

## UART

Initialised to 115200 baud, 8N1, FIFO enabled.  The driver supports two
modes:

- **Polled** (default): `hal_uart_getchar` spins on the Line Status Register.
- **IRQ-driven**: after `hal_uart_rx_init()` is called, received bytes are
  stored in a ring buffer and waiting threads are woken via `sched_wake`.

UART IRQ is IRQ4 (COM1).  The receive ring buffer is `UART_RX_BUF_SIZE = 256`
bytes; on overflow the newest byte is silently dropped.

---

## Memory map parsing

`hal_get_mem_size(mb_info)` parses the multiboot memory map from the
bootloader.  It stores entries in `mmap_entries[]` (up to 32) and returns
the highest usable physical address found.  `hal_get_mmap_entries()` lets
the PMM consume these entries later.

If `mb_info == 0` (Xen PVH path sets it to 0), it assumes 512 MB.

---

## `interrupt_handler`

The central C-level interrupt dispatcher called from `isr_common_handler`:

| Vector | Action |
|--------|--------|
| 32 | Increment `timer_ticks` |
| 32–47 (IRQs) | Call registered handler, send EOI |
| 128 | Call `syscall_handler` |
| 14 | Print "PAGE FAULT at addr rip=... error=...", `kpanic` |
| 13 | Print "GP FAULT rip=... error=...", `kpanic` |
| 8 | Print "DOUBLE FAULT rip=...", hang with `cli;hlt` forever |
| other | Print "UNHANDLED INTERRUPT", `kpanic` |

---

## Power management stubs

`hal_poweroff()` tries three QEMU/Bochs ACPI poweroff I/O sequences.
`hal_reboot()` pulses the keyboard controller reset line (`0x64` / `0xFE`).
