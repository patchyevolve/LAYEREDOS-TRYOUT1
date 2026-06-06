# eventbus.c — Publish/Subscribe Event Bus

**Path:** `os/src/kernel/eventbus.c`  
**Layer:** Cross-cutting (horizontal concern across layers 1–5)

---

## Purpose

Provides a decoupled, asynchronous publish/subscribe mechanism.  Any layer
can publish an event without knowing who is listening.  Subscribers register
callbacks for specific event types.  Events are queued and dispatched
from the idle thread, keeping ISR latency low.

---

## Internal data structures

```c
// Up to 32 active subscriptions
static subscriber_t subscribers[MAX_SUBSCRIBERS];  // MAX = 32

// FIFO ring buffer of pending events
static pending_event_t pending_events[MAX_PENDING_EVENTS];  // MAX = 64
static int queue_head;  // dequeue index
static int queue_tail;  // enqueue index
static int pending_count;
static spinlock_t eventbus_lock;
```

Each `pending_event_t` holds an `event_t` and a `valid` flag.  The ring
buffer is bounded at 64 slots.  Events published when the buffer is full
are silently dropped (but `event_count` still increments).

---

## `event_t` structure

```c
typedef struct {
    event_type_t type;
    uint64_t     timestamp_ns;  // hal_timer_get_ns() at publish time
    uint64_t     arg1, arg2, arg3, arg4;  // caller-defined payload
} event_t;
```

The four `argN` fields are untyped — their meaning depends on `event_type_t`.

---

## Event types (`event_type_t`)

| Type | Meaning | Typical args |
|------|---------|--------------|
| `EV_PAGE_FAULT` | Page fault occurred | arg1=fault addr, arg2=rip |
| `EV_PROCESS_DIED` | Thread/process exited | arg1=thread_id, arg2=exit_code |
| `EV_PROCESS_CREATED` | New thread started | arg1=thread_id |
| `EV_DEVICE_READY` | Device driver attached | arg1=driver_id |
| `EV_DEVICE_FAILED` | Device driver failed | arg1=driver_id |
| `EV_OOM_KILL` | OOM killer fired | arg1=victim_tid |
| `EV_WATCHDOG` | Watchdog health report | arg1=layer_id, arg2=health, arg3=fail_count |
| `EV_MODE_CHANGE` | NORMAL/SAFE/EMERGENCY mode change | arg1=new_mode |
| `EV_TIMER_SKEW` | Timer skew detected | arg1=expected_ns, arg2=actual_ns |
| `EV_USER_EVENT` | User-defined (test/shell) | user-defined |

---

## `eventbus_publish`

```c
spinlock_acquire(&eventbus_lock);
if (pending_count < MAX_PENDING_EVENTS) {
    pending_events[queue_tail] = { type, timestamp, args... };
    pending_events[queue_tail].valid = 1;
    queue_tail = (queue_tail + 1) % MAX_PENDING_EVENTS;
    pending_count++;
}
event_count++;
spinlock_release(&eventbus_lock);
```

Safe to call from **interrupt context** because it only uses the spinlock
(which disables interrupts).  If the buffer is full, the event is dropped
but the count still increments — `eventbus_count()` will show more events
than were actually delivered.

---

## `eventbus_dispatch`

Called from the idle thread loop.  Processes all currently-pending events
**without holding the lock during callback execution** (the C15 fix):

```c
// 1. Snapshot how many events to process (under lock)
spinlock_acquire; to_process = pending_count; spinlock_release;

// 2. For each event:
while (to_process--) {
    spinlock_acquire;
    ev = pending_events[queue_head].event;   // copy out
    pending_events[queue_head].valid = 0;
    queue_head = (queue_head + 1) % MAX;
    pending_count--;
    spinlock_release;                        // release BEFORE callbacks

    fire_callbacks(&ev);                     // called without lock
}
```

`fire_callbacks` iterates `subscribers[]` linearly.  Because the lock is
not held, a subscriber that calls `eventbus_publish` will not deadlock.
However, a subscriber that calls `eventbus_unsubscribe` during iteration
creates a minor race (the unsubscribed entry might still be seen in this
pass).  Acceptable for single-CPU use.

---

## `eventbus_subscribe` / `eventbus_unsubscribe`

Both scan `subscribers[]` linearly under the spinlock.  Maximum 32
simultaneous subscriptions.  Unsubscribe matches on all three of
`(type, callback, context)` — the same callback with different context
pointers is treated as a different subscription.

---

## Known issues (from AUDIT.md)

| Issue | Status |
|-------|--------|
| C15 — callbacks called while holding lock → deadlock | Fixed: lock released before `fire_callbacks` |
| C19 — ring buffer index formula overwrites valid events | Fixed: proper `head`/`tail` ring buffer with `valid` flag |
| Subscriber iteration is racy during unsubscribe | Known, acceptable for UP |
