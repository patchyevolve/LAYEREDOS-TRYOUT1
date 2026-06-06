# watchdog.c — Health Monitor Daemon

**Path:** `os/src/kernel/watchdog.c`  
**Layer:** Cross-cutting (polls all layers)

---

## Purpose

Implements a periodic health-checking system.  Each registered layer
provides a `check()` callback that returns `WATCHDOG_OK`, `WATCHDOG_DEGRADED`,
or `WATCHDOG_FAILED`.  The watchdog tracks consecutive failures and panics
if a layer fails `WATCHDOG_MAX_FAILURES` (3) times in a row.

---

## Registered layers

`watchdog_init` registers three built-in checks:

### HAL health check (layer ID 1)

- **Timer stalled:** If `hal_timer_get_ticks()` returns the same value as
  last check AND ticks > 10 (system has been running long enough), the
  timer IRQ has stopped → `WATCHDOG_FAILED`.
- **Low memory:** If free pages < 5% of total → `WATCHDOG_DEGRADED`.
- Otherwise: updates `last_watchdog_tick` → `WATCHDOG_OK`.

### Scheduler health check (layer ID 2)

- `!sched_running` → `WATCHDOG_FAILED`
- `!current_thread` → `WATCHDOG_FAILED`
- Otherwise: `WATCHDOG_OK`

### PMM health check (layer ID 4)

- `total_pages == 0` → `WATCHDOG_FAILED` (PMM not initialised)
- `free_pages == 0` → `WATCHDOG_FAILED` (OOM)
- `free_pages < 10% of total` → `WATCHDOG_DEGRADED`
- Otherwise: `WATCHDOG_OK`

---

## `watchdog_run`

Called once per second from `watchdog_timer_handler` (every 1000 ticks at
1000 Hz).  For each registered layer:

- `WATCHDOG_OK` → reset `consecutive_failures = 0` (silently)
- `WATCHDOG_DEGRADED` → reset consecutive failures, write a degraded
  message to `watchdog_buf`
- `WATCHDOG_FAILED` → increment `consecutive_failures` and `total_failures`,
  publish `EV_WATCHDOG` event, write a failure message to `watchdog_buf`.
  If `consecutive_failures >= WATCHDOG_MAX_FAILURES` (3): flush buffer,
  call `kpanic("Watchdog layer failure")`.

---

## Deferred I/O: `watchdog_buf`

The watchdog cannot call `kprintf` directly because `watchdog_run` is called
from the timer ISR context (via `watchdog_timer_handler`).  `kprintf`
ultimately calls `kputchar` which does port I/O — safe from ISR, but the
UART might not be ready for multi-character output without busy-waiting.

Instead, messages are written to a 512-byte static buffer `watchdog_buf`
under `hal_save_irq`.  `watchdog_flush()` is called from the idle thread
loop to drain and print the buffer.  This ensures UART output happens from
a thread context, not an ISR.

---

## `watchdog_timer_handler`

Registered with `hal_irq_register(0, watchdog_timer_handler, NULL)` in
`main.c`.  This makes it the **IRQ 0 (timer) handler**.

```c
void watchdog_timer_handler(int_frame_t* frame, void* data) {
    sched_timer_tick();               // tick the scheduler
    static uint64_t last_check = 0;
    uint64_t now = hal_timer_get_ticks();
    if (now - last_check >= 1000) {   // every 1000 ticks = 1 second
        last_check = now;
        watchdog_run();               // run all layer health checks
    }
}
```

`sched_timer_tick()` is called here rather than in `interrupt_handler`
directly, so the watchdog and scheduler are both triggered by the same IRQ
with the watchdog as the dispatcher.

---

## `watchdog_register_layer`

```c
void watchdog_register_layer(int id, const char* name,
                             watchdog_health_t (*check)(char*, size_t));
```

Up to `MAX_WATCHDOG_LAYERS` (16) layers can be registered.  The `check`
function writes a human-readable reason string into the provided buffer on
any non-OK status.

---

## `watchdog_layer_t`

```c
typedef struct {
    int layer_id;
    const char* name;
    watchdog_health_t (*check)(char* reason, size_t len);
    int consecutive_failures;
    int total_failures;
} watchdog_layer_t;
```

`consecutive_failures` is reset to 0 on any non-FAILED result.  This means
a layer can flap between DEGRADED and OK indefinitely without triggering a
panic — only sustained FAILED results escalate.
