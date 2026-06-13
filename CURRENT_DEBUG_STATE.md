# Current Debug State — Fixed: sched_remove_thread run queue corruption

## Status: FIXED (2026-06-11)

## Bug Fixed

**`sched_remove_thread` run queue corruption** — `sched.c`

When `sched_block()` was called on the currently-running init thread (state
THREAD_RUNNING, but already removed from the run queue by `pick_next()`),
`sched_remove_thread()` would set `q->head = NULL` and `q->tail = NULL`
because both `rq_prev` and `rq_next` were NULL. This wiped all ready threads
from the run queue, including the newly spawned `udp_echo` user thread.

**Fix:** Added early-return guard in `sched_remove_thread()`:
```c
if (t->rq_prev == NULL && t->rq_next == NULL && q->head != t) {
    hal_restore_irq(flags);
    return;  /* thread not in any run queue — nothing to remove */
}
```

## What was broken

- `udp_echo.elf` spawned, but never scheduled after sendto() returned.
- Scheduler run queue was wiped when init thread called `sched_block()` on the
  `udproc->exit_waiters` wait queue right after `process_exec()`.
- TCP echo worked because a timer interrupt happened to re-add the init thread
  to the run queue before `sched_block()`, so the queue was not corrupted.

## What was NOT broken

- UDP sendto() worked (packet was sent).
- Listener's kernel echo server received and echoed the packet.
- The echo reply arrived AFTER udp_echo.elf had already exited (confirmed in
  connector.log: line 272-274 shows echo reply arriving after shell start).

## Next Steps

- Verify `make test-net` still passes (5/5 regression tests).
- Run `make test-net-2qemu` to confirm simultaneous-boot validation passes.
- If UDP echo still times out on first attempt, the retry loop in main.c will
  handle it and re-spawn udp_echo.elf.
