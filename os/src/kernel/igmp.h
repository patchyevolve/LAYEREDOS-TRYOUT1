#ifndef IGMP_H
#define IGMP_H

#include "types.h"

#define IGMP_MAX_GROUPS     8

#define IGMP_TYPE_QUERY    0x11
#define IGMP_TYPE_REPORT   0x16
#define IGMP_TYPE_LEAVE    0x17

#define IGMP_ALL_ROUTERS  0xE0000002 /* 224.0.0.2 */
#define IGMP_ALL_HOSTS    0xE0000001 /* 224.0.0.1 */

void igmp_init(void);
int  igmp_mcast_join(uint32_t group_addr);
int  igmp_mcast_leave(uint32_t group_addr);
int  igmp_mcast_is_member(uint32_t group_addr);

#endif
