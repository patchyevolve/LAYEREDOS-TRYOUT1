# keyboard.c — PS/2 Keyboard Driver

**Path:** `os/src/kernel/keyboard.c`  
**Layer:** Layer 1 (HAL) — device driver

---

## Purpose

Receives PS/2 keyboard scancodes via IRQ1, translates them to ASCII
characters using a static scancode map, and stores them in a ring buffer.
Provides a blocking `keyboard_getchar()` for consumers (the `kbtest` shell
command).

---

## Hardware

| Port | Description |
|------|-------------|
| `0x60` | PS2_DATA — read scancode / write command |
| `0x64` | PS2_STATUS / PS2_CMD — status register |
| IRQ 1 | Keyboard interrupt |

---

## Scancode maps

Two 128-entry arrays covering all standard Set 1 scancodes (0x00–0x7F):

- `kc_map[128]` — normal (unshifted) characters
- `kc_map_shift[128]` — shifted characters

Entry 0 = unused, entries for non-printable keys (arrows, F-keys, etc.)
= 0 (ignored).

---

## `keyboard_irq_handler`

Called on every IRQ1.  Reads one byte from `PS2_DATA`:

```
scancode = inb(0x60)
released = scancode & 0x80   // high bit = key release
key = scancode & 0x7F        // low 7 bits = key number
```

Special keys handled:
- `0x2A` / `0x36` (Left/Right Shift): toggle `shift_pressed`
- `0x1D` (Ctrl): toggle `ctrl_pressed` (stored, not yet used)
- `0x38` (Alt): toggle `alt_pressed` (stored, not yet used)

For key-press events (`!released`), the ASCII character is looked up in
the appropriate map.  If non-zero, it is pushed into the ring buffer
`keybuf[]` (dropped silently on overflow).

---

## `keyboard_init`

1. Resets head/tail to 0.
2. Flushes any pending PS/2 data (reads until `PS2_STATUS & 1` is clear).
3. Registers `keyboard_irq_handler` on IRQ1 via `hal_irq_register`.

---

## Ring buffer

```c
static volatile char keybuf[KEYBUF_SIZE];  // KEYBUF_SIZE = 256
static volatile int  keybuf_head;  // next write position
static volatile int  keybuf_tail;  // next read position
```

`head == tail` means empty.  `(head + 1) % SIZE == tail` means full
(one slot wasted to distinguish full from empty).

---

## `keyboard_getchar`

Busy-waits using `sti; hlt; cli` — enables interrupts, halts until any
interrupt fires (which will be the keyboard IRQ), then disables interrupts
and checks the buffer again.  Returns the next ASCII character.

**Note:** This is a coarse blocking mechanism.  It will wake on *any*
interrupt (timer, UART), not just keyboard.  A production implementation
would use `sched_block` on a wait queue woken by the keyboard ISR.

---

## Current limitations

- No Escape sequence parsing (no arrow key support).
- No Num Lock / Caps Lock state.
- Ctrl+C does not send SIGINT (Ctrl is tracked but not acted on).
- No key repeat — each physical key press produces exactly one character.
- Only Set 1 scancodes; extended scancodes (0xE0 prefix) are not handled.
