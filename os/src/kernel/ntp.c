#include "kernel.h"
#include "ntp.h"
#include "udp.h"
#include "route.h"
#include "hal.h"
#include "sched.h"

static int ntp_initialized = 0;
static uint64_t ntp_boot_time = 0;
static uint8_t ntp_server_v4[4] = {216, 239, 35, 0};
static uint16_t ntp_next_port = 40000;

void ntp_set_server_v4(const uint8_t* addr) {
    kmemcpy(ntp_server_v4, addr, 4);
}

uint64_t ntp_get_time(void) {
    if (ntp_boot_time == 0) return 0;
    uint64_t ns = hal_timer_get_ns();
    return ntp_boot_time + (ns / 1000000000ULL);
}

static int ntp_build_request(uint8_t* buf) {
    kmemset(buf, 0, NTP_PACKET_SIZE);
    buf[0] = 0x23;
    return NTP_PACKET_SIZE;
}

static int ntp_parse_response(const uint8_t* resp, int rlen, uint64_t* out_time) {
    if (rlen < NTP_PACKET_SIZE) return ERR_INVAL;
    uint8_t vn = (resp[0] >> 3) & 0x07;
    uint8_t mode = resp[0] & 0x07;
    if (vn < 3 || vn > 4) return ERR_AGAIN;
    if (mode != 4) return ERR_AGAIN;
    uint32_t sec_hi = ((uint32_t)resp[40] << 24) |
                      ((uint32_t)resp[41] << 16) |
                      ((uint32_t)resp[42] << 8)  |
                      (uint32_t)resp[43];
    if (sec_hi < NTP_EPOCH_OFFSET) return ERR_AGAIN;
    *out_time = (uint64_t)(sec_hi - NTP_EPOCH_OFFSET);
    return ERR_OK;
}

void ntp_init(void) {
    if (ntp_initialized) return;
    ntp_initialized = 1;

    uint8_t req[NTP_PACKET_SIZE];
    uint8_t resp[NTP_PACKET_SIZE];
    int qlen = ntp_build_request(req);
    if (qlen <= 0) {
        kprintf("[NTP] Failed to build request\n");
        return;
    }

    uint16_t port = ntp_next_port++;
    int e = udp_bind_endpoint(AF_INET, NULL, port, NTP_TIMEOUT_MS, 0, 1);
    if (e == ERR_NOSPACE) {
        port = ntp_next_port++;
        e = udp_bind_endpoint(AF_INET, NULL, port, NTP_TIMEOUT_MS, 0, 1);
    }
    if (e != ERR_OK) {
        kprintf("[NTP] Bind failed: %d\n", e);
        return;
    }

    udp_endpoint_t* ep = udp_find_endpoint(AF_INET, port);

    uint64_t result_time = 0;
    for (int retry = 0; retry <= NTP_MAX_RETRIES && result_time == 0; retry++) {
        e = udp_sendto(AF_INET, ntp_server_v4, NTP_PORT, port, req, qlen);
        if (e < 0) {
            kprintf("[NTP] Send failed: %d\n", e);
            continue;
        }

        int remaining = NTP_TIMEOUT_MS;
        while (remaining > 0 && result_time == 0) {
            int rlen = udp_endpoint_dequeue(ep, resp, sizeof(resp),
                                             NULL, NULL, NULL, remaining);
            if (rlen <= 0) break;

            e = ntp_parse_response(resp, rlen, &result_time);
            if (e == ERR_OK && result_time > 0) break;
            if (e == ERR_AGAIN) {
                remaining -= 100;
                continue;
            }
            break;
        }
    }

    udp_unbind_endpoint(ep);

    if (result_time > 0) {
        uint64_t ns = hal_timer_get_ns();
        uint64_t uptime_s = ns / 1000000000ULL;
        ntp_boot_time = result_time - uptime_s;

        uint64_t unix_year = 1970 + result_time / 31556926ULL;
        kprintf("[NTP] Clock synchronized: %llu (epoch %llu, ~year %llu)\n",
                result_time, ntp_boot_time, unix_year);
    } else {
        kprintf("[NTP] Clock sync failed (no network or no NTP server)\n");
    }
}
