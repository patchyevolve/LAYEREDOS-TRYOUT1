# eventbus.h — Event Bus Interface

**Path:** `os/src/kernel/eventbus.h`  
**Layer:** Cross-cutting — header

---

## Purpose

Declares the event bus types and API so any layer can publish or subscribe
without including the implementation details.

---

## Types

### `event_type_t`

An enum of all known event categories.  `EV_NONE = 0` is unused.
`EV_MAX` is the sentinel — used to validate that a published type is in
range.  Adding a new event type requires:
1. Inserting it before `EV_MAX` in the enum.
2. Documenting its `argN` semantics.
3. Subscribing a handler somewhere, or the event is published but ignored.

### `event_t`

```c
typedef struct {
    event_type_t type;
    uint64_t     timestamp_ns;
    uint64_t     arg1, arg2, arg3, arg4;
} event_t;
```

Passed by pointer to subscriber callbacks.  The pointer is valid only
for the duration of the callback (it points to a stack-local copy made
during dispatch).

### `event_callback_t`

```c
typedef void (*event_callback_t)(event_t* event, void* context);
```

The `context` pointer is whatever was passed to `eventbus_subscribe`.
Callbacks must not block (they run in the idle thread, but can call
`eventbus_publish` safely).

---

## API summary

| Function | Description |
|----------|-------------|
| `eventbus_init()` | Zero subscriber/queue state, init spinlock |
| `eventbus_publish(type, a1, a2, a3, a4)` | Enqueue an event (safe from ISR) |
| `eventbus_dispatch()` | Drain queue, call callbacks (idle thread only) |
| `eventbus_count()` | Total events ever published (monotonic counter) |
| `eventbus_subscribe(type, cb, ctx)` | Register a callback |
| `eventbus_unsubscribe(type, cb, ctx)` | Remove a callback |

---

## Architectural role

The event bus is a **horizontal** component — it connects layers without
creating a dependency edge in the vertical layer graph.  For example:

- The watchdog publishes `EV_WATCHDOG` (Layer 1/2) which could be
  consumed by a future UI layer without watchdog.c knowing about it.
- The PMM could publish `EV_OOM_KILL` which the scheduler could subscribe
  to without the PMM directly calling scheduler functions.

This keeps the strict N → N-1 layering intact while allowing cross-layer
notification.
