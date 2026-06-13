#ifndef NTP_H
#define NTP_H

#include "types.h"

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_TIMEOUT_MS 5000
#define NTP_MAX_RETRIES 2

#define NTP_EPOCH_OFFSET 2208988800ULL

uint64_t ntp_get_time(void);
void ntp_set_server_v4(const uint8_t* addr);
void ntp_init(void);

#endif
