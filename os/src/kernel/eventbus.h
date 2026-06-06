#ifndef EVENTBUS_H
#define EVENTBUS_H

#include "types.h"

typedef enum {
    EV_NONE = 0,
    EV_PAGE_FAULT,
    EV_PROCESS_DIED,
    EV_PROCESS_CREATED,
    EV_DEVICE_READY,
    EV_DEVICE_FAILED,
    EV_OOM_KILL,
    EV_WATCHDOG,
    EV_MODE_CHANGE,
    EV_TIMER_SKEW,
    EV_USER_EVENT,
    EV_MAX
} event_type_t;

typedef struct {
    event_type_t type;
    uint64_t     timestamp_ns;
    uint64_t     arg1;
    uint64_t     arg2;
    uint64_t     arg3;
    uint64_t     arg4;
} event_t;

typedef void (*event_callback_t)(event_t* event, void* context);

err_t eventbus_init(void);
err_t eventbus_publish(event_type_t type, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4);
void  eventbus_dispatch(void);
uint64_t eventbus_count(void);
err_t eventbus_subscribe(event_type_t type, event_callback_t callback,
                         void* context);
err_t eventbus_unsubscribe(event_type_t type, event_callback_t callback,
                           void* context);

#endif
