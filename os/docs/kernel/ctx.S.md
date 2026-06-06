# ctx.S — Context Switch and Thread Trampoline

**Path:** `os/src/kernel/ctx.S`  
**Layer:** Layer 2 (Scheduler) — assembly back-end  
**Language:** x86-64 AT&T assembly

---

## Purpose

Contains three hand-written assembly routines that cannot be expressed in
portable C:

1. **`switch_context`** — saves the current thread's callee-saved registers
   and stack pointer, then restores them from the next thread.
2. **`thread_trampoline`** — the first function that runs in a newly created
   kernel thread; calls the thread's function and then `thread_exit`.
3. **`user_thread_entry`** — the first thing that runs in a new user-mode
   thread's kernel stack; pops a saved register frame and executes `iretq`
   to drop into ring 3.

---

## `switch_context(thread_t** current, thread_t** next)`

### Calling convention

| Register | Role |
|----------|------|
| `%rdi` | `thread_t** current` — pointer to the `current_thread` pointer |
| `%rsi` | `thread_t** next` — pointer to the new `current_thread` pointer |

Both arguments are **pointers to pointers**.  The caller (`schedule()`)
passes `&old` and `&current_thread` so that after the switch completes in
the new thread's context, `current_thread` already holds the correct value.

### Sequence

```asm
; ── save current thread ──────────────────────────────
pushq %r15 / %r14 / %r13 / %r12 / %rbx / %rbp
  ; these are the 6 callee-saved registers the C ABI requires us to preserve

movq (%rdi), %rdi       ; dereference: %rdi = *current = old thread_t*
movq %rsp, (%rdi)       ; old->rsp = RSP   (offset 0 of thread_t)

; ── restore next thread ──────────────────────────────
movq (%rsi), %rsi       ; dereference: %rsi = *next = new thread_t*
movq (%rsi), %rsp       ; RSP = new->rsp   (offset 0 of thread_t)

popq %rbp / %rbx / %r12 / %r13 / %r14 / %r15
ret                     ; pops RIP → jumps into new thread
```

### Why offset 0?

`thread_t` is declared in `sched.h`:

```c
typedef struct thread {
    uint64_t id;     // offset 0  ← NO — wait:
    uint64_t rsp;    // this is actually second?
```

Actually the layout is:

```c
struct thread {
    uint64_t  id;    // +0
    uint64_t  rsp;   // +8   ← WAIT, let me re-read...
```

Actually in the current `sched.h`:

```c
typedef struct thread {
    uint64_t            id;          // +0
    uint64_t            rsp;         // +8
    uint64_t            cr3;         // +16
    ...
```

So `rsp` is at **offset 8**, not 0.

**But the assembly uses `(%rdi)` / `(%rsi)` — offset 0.**

This is the known offset bug described in the project progress notes.
The fix is to change the `movq %rsp, (%rdi)` / `movq (%rsi), %rsp` lines
to `8(%rdi)` / `8(%rsi)` to match the actual `thread_t.rsp` field position,
OR to reorder the `thread_t` struct so `rsp` is the first member (offset 0).

The current code compiles and the bug manifests as: on the very first
`switch_context` call, RSP is saved to `thread->id` (overwriting the thread
ID with the stack pointer value) and loaded from the wrong address.

---

### What the new thread resumes at

When `ret` pops RIP, it continues wherever the new thread last called
`switch_context` from — either:

- Another point inside `schedule()` if it was previously preempted, or
- `thread_trampoline` (for a brand-new thread — the stack was built by
  `thread_create` to have `thread_trampoline` at the `ret` address).

---

## `thread_trampoline`

The entry point for every newly-created kernel thread.  `thread_create`
builds a fake switch-context frame on the new thread's kernel stack:

```
[top of stack]
  return addr → thread_trampoline
  r15         = func   (thread function pointer)
  r14         = arg    (thread argument)
  r13, r12, rbp, rbx = 0
```

When `switch_context` does its `ret`, execution lands here:

```asm
thread_trampoline:
    testq %r15, %r15
    jz    .Lnull_func_panic     ; guard against NULL func (init thread)
    movq  %r14, %rdi            ; arg → first argument register
    call  *%r15                 ; call func(arg)
    movq  %rax, %rdi            ; return value → exit_code
    call  thread_exit
    int   $0x20                 ; should never reach here
.Lnull_func_panic:
    movabsq $null_func_msg, %rdi
    call  kpanic
```

The null check is important for the `init` thread created by `sched_init`,
which has `func=NULL`.  Without it, `call *%r15` would jump to address 0
causing a page fault.

---

## `user_thread_entry`

The entry point for user-mode threads.  `thread_create_user` builds the
kernel stack with:

```
[switch_context frame]   6 zeros + user_thread_entry as return addr
[15 zero GP registers]   will be popped: rax,rbx,...,r15
[error code = 0]         \__ skipped by addq $16
[vector = 0]             /
[RIP  = 0x40000000]      \
[CS   = 0x1B]             |__ iretq frame (CPU restores these)
[RFLAGS = 0x202]          |
[RSP  = 0x70001000]       |
[SS   = 0x23]            /
```

Execution:

```asm
user_thread_entry:
    popq %rax    ; pop 15 GP registers (all zeros)
    ...
    popq %r15
    addq $16, %rsp   ; skip vector + error_code fields
    iretq            ; jumps to user RIP=0x40000000, ring 3
```

After `iretq` the CPU:
- Loads `CS = 0x1B` → switches to ring 3
- Loads `SS = 0x23` → switches to user data segment
- Loads `RSP = 0x70001000` → user stack
- Loads `RFLAGS = 0x202` → enables interrupts (IF=1)
- Jumps to `RIP = 0x40000000` → first instruction of user program

---

## `.rodata` section

```asm
null_func_msg: .asciz "thread_trampoline: NULL function pointer"
```

Placed in `.rodata` so it does not occupy writable memory.
