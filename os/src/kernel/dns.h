#ifndef DNS_H
#define DNS_H

#include "types.h"

#define DNS_PORT          53
#define DNS_MAX_PACKET    512
#define DNS_TYPE_A        1
#define DNS_TYPE_AAAA     28
#define DNS_CLASS_IN      1

void dns_init(void);
int dns_resolve(const char* hostname, uint8_t* addr_out, int* af_out, int timeout_ms);

#endif
