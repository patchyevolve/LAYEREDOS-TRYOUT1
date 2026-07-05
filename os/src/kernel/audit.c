#include "kernel.h"
#include "security.h"
#include "sched.h"
#include "hal.h"
#include "sync.h"
#include "process.h"

/* Fixed-size ring buffer for audit events */
static audit_entry_t audit_buffer[AUDIT_BUFFER_SIZE];
static unsigned int audit_head;   /* next write position */
static unsigned int audit_tail;   /* oldest unread position */
static unsigned int audit_count;  /* number of entries in buffer */
static spinlock_t audit_lock;

void audit_init(void) {
    spinlock_init(&audit_lock, "audit_lock");
    audit_head = 0;
    audit_tail = 0;
    audit_count = 0;
    kmemset(audit_buffer, 0, sizeof(audit_buffer));
}

void audit_log(int event_type, const char* data) {
    cpu_flags_t flags;
    spinlock_acquire(&audit_lock, &flags);

    unsigned int next = (audit_head + 1) % AUDIT_BUFFER_SIZE;

    /* Overwrite oldest if full */
    if (audit_count == AUDIT_BUFFER_SIZE) {
        audit_tail = (audit_tail + 1) % AUDIT_BUFFER_SIZE;
        audit_count--;
    }

    audit_entry_t* e = &audit_buffer[audit_head];
    e->timestamp_ms = hal_timer_get_ns() / 1000000ULL;
    e->event_type = event_type;
    e->pid = current_thread ? current_thread->proc->pid : 0;
    kstrncpy(e->data, data ? data : "", AUDIT_DATA_LEN - 1);
    e->data[AUDIT_DATA_LEN - 1] = 0;

    audit_head = next;
    audit_count++;
    spinlock_release(&audit_lock, flags);
}

int audit_read_next(audit_entry_t* entry) {
    if (!entry) return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&audit_lock, &flags);

    if (audit_count == 0) {
        spinlock_release(&audit_lock, flags);
        return ERR_NOENT;
    }

    kmemcpy(entry, &audit_buffer[audit_tail], sizeof(audit_entry_t));
    audit_tail = (audit_tail + 1) % AUDIT_BUFFER_SIZE;
    audit_count--;

    spinlock_release(&audit_lock, flags);
    return ERR_OK;
}

int cap_check(uint64_t required_cap) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    if (proc->caps & required_cap) return 1;
    audit_log(AUDIT_CAP_DENIED, "cap_check denied");
    return 0;
}

void cap_init(void) {
    audit_init();
}
