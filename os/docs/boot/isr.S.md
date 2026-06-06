# isr.S — Interrupt Service Routine Stub Table

**Path:** `os/src/boot/isr.S`  
**Layer:** Layer 1 (HAL) — interrupt entry glue  
**Language:** x86-64 AT&T assembly

---

## Purpose

Provides one small assembly stub per interrupt vector.  Each stub does the
minimum necessary to produce a **uniform stack frame** before jumping to the
shared C handler `interrupt_handler()`.  It also checks `need_reschedule`
after the handler returns and calls `schedule()` if a context switch is
pending.

---

## The uniform-frame problem

The x86-64 CPU pushes different things onto the kernel stack depending on
the exception:

- **Exceptions with an error code** (e.g., #GP=13, #PF=14, #DF=8):
  CPU pushes `SS, RSP, RFLAGS, CS, RIP, error_code`
- **Exceptions without an error code** (e.g., #DE=0, timer IRQ=32):
  CPU pushes `SS, RSP, RFLAGS, CS, RIP` — no error code

The C handler `interrupt_handler(int_frame_t*)` expects a single struct
layout.  The stubs normalise the two cases:

```
ISR_NOERR num:   pushq $0       ← fake zero error code
                 pushq $num     ← vector number
                 jmp isr_common_handler

ISR_ERR   num:   pushq $num     ← vector number (error code already on stack)
                 jmp isr_common_handler
```

After both macros the stack looks like:

```
[RSP+0]   vector number
[RSP+8]   error code (real or zero)
[RSP+16]  RIP   (pushed by CPU)
[RSP+24]  CS
[RSP+32]  RFLAGS
[RSP+40]  RSP   (user/previous kernel RSP)
[RSP+48]  SS
```

---

## isr_common_handler

Saves all 15 general-purpose registers (in the order `int_frame_t` expects)
then calls the C handler:

```asm
pushq %r15 .. %rax          ; 15 GP regs (rax last → lowest address)
movq  %rsp, %rdi            ; pass pointer to the whole frame as arg1
cld                         ; direction flag clear (C ABI)
call  interrupt_handler
```

After `interrupt_handler` returns:

```asm
cmpl  $0, need_reschedule(%rip)
je    1f
call  schedule              ; preempt if timer tick set the flag
1:
popq  %rax .. %r15          ; restore GP regs
addq  $16, %rsp             ; skip vector + error_code fields
iretq                       ; restore RIP, CS, RFLAGS, RSP, SS
```

**Why call `schedule()` here?**  
The timer ISR (vector 32) sets `need_reschedule = 1` via
`sched_timer_tick()`.  Calling `schedule()` while the registers are still
on the stack means the context switch saves the ISR-exit frame of the
*current* thread and restores the frame of the *next* thread.  When the
next thread's ISR exit path runs, it pops *its* registers and `iretq`s
back to wherever that thread was interrupted — a complete, correct
preemptive switch.

---

## Vector assignments

| Range | Macro | Vectors |
|-------|-------|---------|
| 0–7 | `ISR_NOERR` | #DE, #DB, NMI, #BP, #OF, #BR, #UD, #NM |
| 8 | `ISR_ERR` | #DF — double fault (has error code 0, but CPU still pushes it) |
| 9 | `ISR_NOERR` | Coprocessor segment overrun (legacy) |
| 10–14 | `ISR_ERR` | #TS, #NP, #SS, #GP, #PF — all push error codes |
| 15–20 | `ISR_NOERR` | Reserved / #MF / #AC stub / #MC / #XF / #VE |
| 21–31 | `ISR_NOERR` | Reserved |
| 32–47 | `ISR_NOERR` | Hardware IRQs remapped to PIC vectors 32–47 |
| 128 | `ISR_NOERR` | `int $0x80` — software syscall gateway (DPL=3 in IDT) |

Vectors 48–127 and 129–255 are not defined; their IDT entries remain zeroed
(not-present), so any spurious delivery generates a #GP rather than a
silent miss.

---

## isr_vectors table

At the end of the file (`.rodata` section) a 256-entry table of 8-byte
function pointers is exported as `isr_vectors[]`:

```asm
isr_vectors:
  .quad isr0, isr1, ..., isr47
  .zero 8 * 80          ; entries 48–127 are NULL
  .quad isr128
```

`hal_init()` iterates this table and calls `idt_set_gate(i, isr_vectors[i], dpl)`
for every non-NULL entry, installing the real ISR addresses into the IDT.

---

## `int_frame_t` layout (cross-reference with `hal.h`)

```c
typedef struct int_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;   // ← popped first
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15; // ← popped last
    uint64_t vector;        // pushed by ISR stub (vector number)
    uint64_t error_code;    // pushed by ISR stub or CPU
    uint64_t rip, cs, rflags, rsp, ss;  // pushed by CPU on exception
} int_frame_t;
```

The `int_frame_t*` passed to `interrupt_handler` points at `rax` — the
lowest GP register pushed.  All fields are accessible as plain struct
members in C.

---

## IST for double fault (#DF, vector 8)

`idt_set_gate(8, ...)` sets `ist = 1` in the IDT entry.  When a double
fault fires, the CPU automatically switches to the stack at `tss.ist[0]`
(`ist_stack0 + 8192`) instead of the potentially-corrupted kernel stack.
This ensures the #DF handler can always execute even if RSP was bad.
