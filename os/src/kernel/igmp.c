#include "kernel.h"
#include "igmp.h"
#include "ipv4.h"
#include "e1000.h"
#include "route.h"
#include "net_ns.h"
#include "sync.h"

#define IGMP_HDR_SIZE   8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  max_resp;
    uint16_t checksum;
    uint32_t group;
} igmp_hdr_t;

#define igmp_initialized (get_current_ns()->igmp_initialized)
#define igmp_groups (get_current_ns()->igmp_groups)
#define igmp_group_used (get_current_ns()->igmp_group_used)
#define igmp_lock (get_current_ns()->igmp_lock)

static uint16_t igmp_checksum(const void* data, uint32_t len) {
    uint32_t sum = 0;
    const uint16_t* p = (const uint16_t*)data;
    for (uint32_t i = 0; i < len / 2; i++)
        sum += __builtin_bswap16(p[i]);
    if (len & 1)
        sum += ((const uint8_t*)data)[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return __builtin_bswap16(~sum & 0xFFFF);
}

static void igmp_mac_from_ipv4(uint32_t ip, uint8_t* mac) {
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5E;
    mac[3] = (ip >> 16) & 0x7F;
    mac[4] = (ip >> 8) & 0xFF;
    mac[5] = ip & 0xFF;
}

static void igmp_send_report(uint32_t group) {
    uint8_t buf[IGMP_HDR_SIZE];
    igmp_hdr_t* hdr = (igmp_hdr_t*)buf;
    hdr->type = IGMP_TYPE_REPORT;
    hdr->max_resp = 0;
    hdr->checksum = 0;
    hdr->group = __builtin_bswap32(group);
    hdr->checksum = igmp_checksum(buf, IGMP_HDR_SIZE);

    ipv4_addr_t dst = { .bytes = {
        (uint8_t)(group >> 24), (uint8_t)(group >> 16),
        (uint8_t)(group >> 8), (uint8_t)group
    }};
    ipv4_send(dst, 2, buf, IGMP_HDR_SIZE);

    uint8_t mac[6];
    igmp_mac_from_ipv4(group, mac);
    e1000_mta_set(mac);
}

static void igmp_send_leave(uint32_t group) {
    uint8_t buf[IGMP_HDR_SIZE];
    igmp_hdr_t* hdr = (igmp_hdr_t*)buf;
    hdr->type = IGMP_TYPE_LEAVE;
    hdr->max_resp = 0;
    hdr->checksum = 0;
    hdr->group = __builtin_bswap32(group);
    hdr->checksum = igmp_checksum(buf, IGMP_HDR_SIZE);

    ipv4_addr_t dst = { .bytes = {224, 0, 0, 2} };
    ipv4_send(dst, 2, buf, IGMP_HDR_SIZE);
}

int igmp_mcast_join(uint32_t group_addr) {
    if ((group_addr & 0xF0000000) != 0xE0000000)
        return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&igmp_lock, &flags);
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_group_used[i] && igmp_groups[i] == group_addr) {
            spinlock_release(&igmp_lock, flags);
            return ERR_OK;
        }
    }
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (!igmp_group_used[i]) {
            igmp_groups[i] = group_addr;
            igmp_group_used[i] = 1;
            spinlock_release(&igmp_lock, flags);
            igmp_send_report(group_addr);
            return ERR_OK;
        }
    }
    spinlock_release(&igmp_lock, flags);
    return ERR_NOSPACE;
}

int igmp_mcast_leave(uint32_t group_addr) {
    cpu_flags_t flags;
    spinlock_acquire(&igmp_lock, &flags);
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_group_used[i] && igmp_groups[i] == group_addr) {
            igmp_group_used[i] = 0;
            spinlock_release(&igmp_lock, flags);
            igmp_send_leave(group_addr);
            return ERR_OK;
        }
    }
    spinlock_release(&igmp_lock, flags);
    return ERR_NOENT;
}

int igmp_mcast_is_member(uint32_t group_addr) {
    cpu_flags_t flags;
    spinlock_acquire(&igmp_lock, &flags);
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_group_used[i] && igmp_groups[i] == group_addr) {
            spinlock_release(&igmp_lock, flags);
            return 1;
        }
    }
    spinlock_release(&igmp_lock, flags);
    return 0;
}

static void igmp_report_all(void) {
    cpu_flags_t flags;
    spinlock_acquire(&igmp_lock, &flags);
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_group_used[i]) {
            uint32_t g = igmp_groups[i];
            igmp_send_report(g);
        }
    }
    spinlock_release(&igmp_lock, flags);
}

static void igmp_handler(ipv4_addr_t src, ipv4_addr_t dst,
                          uint8_t protocol,
                          const uint8_t* data, uint32_t len) {
    (void)src; (void)dst; (void)protocol;
    if (len < IGMP_HDR_SIZE) return;
    const igmp_hdr_t* hdr = (const igmp_hdr_t*)data;

    if (hdr->type == IGMP_TYPE_QUERY) {
        uint32_t group = __builtin_bswap32(hdr->group);
        if (group == 0) {
            igmp_report_all();
        } else {
            if (igmp_mcast_is_member(group))
                igmp_send_report(group);
        }
    }
}

void igmp_init(void) {
    if (igmp_initialized) return;

    spinlock_init(&igmp_lock, "igmp_lock");
    igmp_initialized = 1;
    ipv4_register_handler(2, igmp_handler);
    kprintf("[IGMP] Initialized\n");
}
