#ifndef SECURITY_H
#define SECURITY_H

#include "types.h"

/* ── Capabilities (bitmask, per process) ── */
#define CAP_SYS_BOOT    (1ULL << 0)
#define CAP_KILL        (1ULL << 1)
#define CAP_NET_RAW     (1ULL << 2)
#define CAP_SYS_ADMIN   (1ULL << 3)
#define CAP_DAC_OVERRIDE (1ULL << 4)
#define CAP_SYS_SETUID  (1ULL << 5)

#define CAP_ALL  (CAP_SYS_BOOT | CAP_KILL | CAP_NET_RAW | CAP_SYS_ADMIN | \
                  CAP_DAC_OVERRIDE | CAP_SYS_SETUID)

#ifndef __ASSEMBLER__

/* ── Audit log ── */
#define AUDIT_BUFFER_SIZE 256
#define AUDIT_DATA_LEN    48

typedef enum {
    AUDIT_PROCESS_EXEC   = 1,
    AUDIT_PROCESS_EXIT   = 2,
    AUDIT_CAP_DENIED     = 3,
    AUDIT_SENSITIVE_CALL = 4,
    AUDIT_FORK_DENIED    = 5,
    AUDIT_NETNS_CREATE   = 6,
} audit_event_type_t;

typedef struct {
    uint64_t      timestamp_ms;
    int           event_type;
    unsigned long pid;
    char          data[AUDIT_DATA_LEN];
} audit_entry_t;

void audit_log(int event_type, const char* data);
int  audit_read_next(audit_entry_t* entry);

/* ── Capability helpers ── */
int  cap_check(uint64_t required_cap);
void cap_init(void);

#endif /* __ASSEMBLER__ */

#endif
