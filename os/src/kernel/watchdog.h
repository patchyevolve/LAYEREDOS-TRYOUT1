#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "types.h"
#include "hal.h"

#define WATCHDOG_INTERVAL_MS 2000
#define WATCHDOG_MAX_FAILURES 3

typedef enum {
    WATCHDOG_OK = 0,
    WATCHDOG_DEGRADED,
    WATCHDOG_FAILED
} watchdog_health_t;

typedef struct {
    int layer_id;
    const char* name;
    watchdog_health_t (*check)(char* reason, size_t len);
    int consecutive_failures;
    int total_failures;
} watchdog_layer_t;

err_t watchdog_init(void);
void watchdog_register_layer(int id, const char* name,
                             watchdog_health_t (*check)(char*, size_t));
void watchdog_run(void);
void watchdog_flush(void);
void watchdog_timer_handler(int_frame_t* frame, void* data);

#endif
