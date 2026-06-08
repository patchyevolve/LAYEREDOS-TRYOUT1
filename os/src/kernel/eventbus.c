#include "kernel.h"
#include "eventbus.h"
#include "hal.h"
#include "sync.h"

#define MAX_SUBSCRIBERS 32
#define MAX_PENDING_EVENTS 64

typedef struct {
    event_type_t      type;
    event_callback_t  callback;
    void*             context;
    int               active;
} subscriber_t;

typedef struct {
    event_t event;
    int     valid;
} pending_event_t;

static subscriber_t subscribers[MAX_SUBSCRIBERS];
static pending_event_t pending_events[MAX_PENDING_EVENTS];
static int queue_head = 0;
static int queue_tail = 0;
static int pending_count = 0;
static spinlock_t eventbus_lock;
static uint64_t event_count = 0;
static int bus_initialized = 0;

err_t eventbus_init(void) {
    kmemset(subscribers, 0, sizeof(subscribers));
    kmemset(pending_events, 0, sizeof(pending_events));
    spinlock_init(&eventbus_lock, "eventbus");
    bus_initialized = 1;
    event_count = 0;
    return ERR_OK;
}

err_t eventbus_publish(event_type_t type, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4) {
    if (!bus_initialized || type >= EV_MAX) return ERR_INVAL;

    cpu_flags_t _sflags; spinlock_acquire(&eventbus_lock, &_sflags);

    if (pending_count < MAX_PENDING_EVENTS) {
        int idx = queue_tail;
        pending_events[idx].valid = 1;
        pending_events[idx].event.type = type;
        pending_events[idx].event.timestamp_ns = hal_timer_get_ns();
        pending_events[idx].event.arg1 = a1;
        pending_events[idx].event.arg2 = a2;
        pending_events[idx].event.arg3 = a3;
        pending_events[idx].event.arg4 = a4;
        queue_tail = (queue_tail + 1) % MAX_PENDING_EVENTS;
        pending_count++;
    }

    event_count++;
    spinlock_release(&eventbus_lock, _sflags);
    return ERR_OK;
}

static void fire_callbacks(const event_t* ev) {
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].active && subscribers[i].type == ev->type)
            subscribers[i].callback((event_t*)ev, subscribers[i].context);
    }
}

void eventbus_dispatch(void) {
    if (!bus_initialized) return;

    cpu_flags_t _sflags;
    spinlock_acquire(&eventbus_lock, &_sflags);
    int to_process = pending_count;
    spinlock_release(&eventbus_lock, _sflags);

    while (to_process > 0) {
        spinlock_acquire(&eventbus_lock, &_sflags);

        if (pending_count == 0) {
            spinlock_release(&eventbus_lock, _sflags);
            break;
        }

        int idx = queue_head;
        if (!pending_events[idx].valid) {
            spinlock_release(&eventbus_lock, _sflags);
            break;
        }

        event_t ev = pending_events[idx].event;
        pending_events[idx].valid = 0;
        queue_head = (queue_head + 1) % MAX_PENDING_EVENTS;
        pending_count--;
        spinlock_release(&eventbus_lock, _sflags);

        fire_callbacks(&ev);

        to_process--;
    }
}

uint64_t eventbus_count(void) { return event_count; }

err_t eventbus_subscribe(event_type_t type, event_callback_t callback,
                         void* context) {
    if (!bus_initialized || type >= EV_MAX || !callback) return ERR_INVAL;

    cpu_flags_t _sflags; spinlock_acquire(&eventbus_lock, &_sflags);

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!subscribers[i].active) {
            subscribers[i].type = type;
            subscribers[i].callback = callback;
            subscribers[i].context = context;
            subscribers[i].active = 1;
            spinlock_release(&eventbus_lock, _sflags);
            return ERR_OK;
        }
    }

    spinlock_release(&eventbus_lock, _sflags);
    return ERR_NOMEM;
}

err_t eventbus_unsubscribe(event_type_t type, event_callback_t callback,
                           void* context) {
    if (!bus_initialized || type >= EV_MAX || !callback) return ERR_INVAL;

    cpu_flags_t _sflags; spinlock_acquire(&eventbus_lock, &_sflags);

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].active &&
            subscribers[i].type == type &&
            subscribers[i].callback == callback &&
            subscribers[i].context == context) {
            subscribers[i].active = 0;
            subscribers[i].callback = NULL;
            subscribers[i].context = NULL;
            spinlock_release(&eventbus_lock, _sflags);
            return ERR_OK;
        }
    }

    spinlock_release(&eventbus_lock, _sflags);
    return ERR_NOENT;
}
