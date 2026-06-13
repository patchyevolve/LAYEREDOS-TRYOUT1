#ifndef NIC_H
#define NIC_H

#include "types.h"
#include "hal.h"

#define NIC_MAC_LEN   6
#define NIC_MAX_FRAME 1518

typedef struct nic {
    uint8_t  mac[NIC_MAC_LEN];
    int      irq;
    int      present;
    err_t  (*send)(const struct nic* nic, const uint8_t* frame, uint32_t len);
    int    (*poll)(const struct nic* nic, uint8_t* buf, uint32_t max_len);
} nic_t;

extern nic_t nic;

err_t nic_init(void);
void nic_handle_irq(int_frame_t* frame, void* data);

#endif
