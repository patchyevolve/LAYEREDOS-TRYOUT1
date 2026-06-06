# watchdog.h — Watchdog Interface

**Path:** `os/src/kernel/watchdog.h`  
**Layer:** Cross-cutting — header

---

## Purpose

Declares the watchdog types and API.  Included by `main.c` (to call
`watchdog_init` and register the timer handler) and `sched.c` / `pmm.c`
indirectly (their health check functions are registered by watchdog.c itself,
so they need no direct watchdog include).

---

## Constants

```c
#define WATCHDOG_INTERVAL_MS  2000   // informational — actual interval is 1s (1000 ticks)
#define WATCHDOG_MAX_FAILURES 3      // consecutive failures before panic
```

---

## `watchdog_health_t`

```c
typedef enum {
    WATCHDOG_OK = 0,
    WATCHDOG_DEGRADED,   // non-fatal, log and continue
    WATCHDOG_FAILED      // serious; 3× in a row → panic
} watchdog_health_t;
```

Check callbacks return this enum.  The description string is only populated
for DEGRADED or FAILED — on OK, the buffer contents are ignored.

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

The `check` function signature is:
```c
watchdog_health_t my_check(char* reason, size_t len);
// Write a reason string into reason[0..len-1] on non-OK status.
// Return the health level.
```

---

## API reference

| Function | Description |
|----------|-------------|
| `watchdog_init()` | Register built-in HAL/sched/PMM checks; set `watchdog_running = 1` |
| `watchdog_register_layer(id, name, check)` | Add a custom layer check (up to 16) |
| `watchdog_run()` | Execute all registered checks; called once per second |
| `watchdog_flush()` | Print deferred watchdog log buffer (call from thread context) |
| `watchdog_timer_handler(frame, data)` | IRQ0 handler: ticks scheduler + runs watchdog |
