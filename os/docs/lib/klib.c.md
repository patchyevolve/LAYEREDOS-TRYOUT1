# klib.c — Kernel Standard Library

**Path:** `os/src/lib/klib.c`  
**Layer:** Cross-cutting (used by all layers)

---

## Purpose

Provides the C standard library functions the kernel needs: character and
string output, formatted printing, memory manipulation, and the panic handler.
All of these are implemented from scratch — no host libc is linked.

---

## Output targets

Every character output goes to **two destinations simultaneously**:

### UART (COM1, 0x3F8)

```c
while (!(inb(UART_LSR) & 0x20));   // wait for transmit-hold-empty
outb(UART_THR, c);
```

Busy-polls the Line Status Register until the transmit buffer is ready.
For `'\n'`, a `'\r'` is sent first (CRLF for terminal emulators).

### VGA text buffer

A classic 80×25 VGA text mode buffer at virtual address
`KERNEL_VMA_BASE + 0xB8000`.  Each cell is 16 bits: low byte = character,
high byte = attribute (colour).  All output uses attribute `0x07`
(white on black).

The VGA state (`vga_row`, `vga_col`) is maintained globally.  `vga_scroll()`
shifts all rows up by one when the cursor reaches the bottom.
`vga_update_cursor()` moves the hardware cursor via the CRT controller
registers (`0x3D4`/`0x3D5`).

---

## `kputchar(char c)`

Core output function.  Handles `'\n'` (CRLF on UART, newline + scroll on VGA),
`'\r'` (carriage return), `'\b'`/DEL (backspace), and printable characters.
All other characters are silently dropped.

---

## `kputs(const char* s)`

Loop over `s` calling `kputchar` until null terminator.  No length limit.

---

## `kprintf(const char* fmt, ...)`

Format string interpreter using `__builtin_va_list`.  Supported specifiers:

| Specifier | Type consumed | Output |
|-----------|--------------|--------|
| `%d` / `%ld` / `%lld` | `int` / `long` / `long long` | Signed decimal via `kprint_int64` |
| `%u` / `%lu` / `%llu` | `unsigned` / ... | Unsigned decimal via `kprint_uint64` |
| `%x` / `%lx` / `%llx` | `unsigned` / ... | Trimmed hex (leading zeros stripped) via `kputhex_trim` |
| `%p` | `void*` | Full 16-digit hex via `kputhex` |
| `%s` | `const char*` | String via `kputs`; `NULL` → `"(null)"` |
| `%c` | `int` | Single character |
| `%%` | — | Literal `%` |

Numeric padding with leading zeros: `%04d` sets `pad=4`.  Width specifier
is parsed before the `l` count.

**No floating point.** No `%f`, `%e`, `%g`.

### `kprint_int64` vs `kprint_uint64`

Two separate functions:
- `kprint_int64(int64_t v, base, pad)` — handles sign, used for `%d`
- `kprint_uint64(uint64_t v, pad)` — always positive, used for `%u`/`%lu`

This fixes the audit bug C20 where `%u` with large values was printed
with a minus sign (previously both paths used the signed function).

### `kputhex` vs `kputhex_trim`

- `kputhex` always prints 16 hex digits (used for `%p` pointer output).
- `kputhex_trim` strips leading zero nibbles (used for `%x`).

---

## Memory utilities

### `kmemset(void* d, int c, size_t n)`

Fills `n` bytes starting at `d` with the byte value `c`.  The `-ffreestanding`
flag prevents the compiler from replacing this with an SSE `memset` that
would require saving/restoring SSE registers in ISR context.

### `kmemcpy(void* d, const void* s, size_t n)`

Copies `n` bytes from `s` to `d`.  No overlap detection — caller's
responsibility.

---

## String utilities

### `kstrcmp(a, b)`

Standard lexicographic comparison.  Returns 0 on equal, positive/negative
on greater/less.  Used by the shell command dispatcher.

### `kstrncpy(d, s, n)`

Copies at most `n` bytes from `s` to `d`, padding with zeros to fill `n`
bytes total (POSIX `strncpy` semantics).  Used for safe thread name copying.

### `kstrlen(s)`

Counts bytes until null terminator.  Not declared in `kernel.h` but used
internally in `klib.c` and `shell.c` (declared in `shell.c` via an implicit
declaration — this should be added to `kernel.h`).

---

## `kputhex` / `kputdec`

Public wrappers around `kputhex` (16-digit hex) and `kprint_int64` as
decimal.  Declared in `kernel.h` for use by early boot code before
`kprintf` is fully set up.

---

## `kpanic(const char* msg, ...)`

```c
__attribute__((noreturn))
void kpanic(const char* msg, ...) {
    // print banner
    // print formatted message (subset: %s %x %d only)
    // kdump_stack()  ← walk RBP chain, print up to 32 frames
    // for(;;) cli; hlt
}
```

`kdump_stack` reads `%rbp` from the current frame and follows `rbp[0]`
(next frame) / `rbp[1]` (return address) links.  Works because the kernel
is compiled with `-fno-omit-frame-pointer`.

Output addresses can be resolved with:
```bash
nm os/build/kernel.elf | sort
# or
objdump -d os/build/kernel.elf | grep <address>
```

---

## Known issues (from AUDIT.md)

| Issue | Status |
|-------|--------|
| C8 — `%x` calls `kprintf` recursively corrupting `va_list` | Fixed: `kputhex_trim` is a direct function, no recursive `kprintf` call |
| C20 — `%u` treats large values as signed | Fixed: separate `kprint_uint64` function |
