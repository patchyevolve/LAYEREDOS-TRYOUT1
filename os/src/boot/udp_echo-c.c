#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>

static int parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void parse_ipv6(const char* s, unsigned char* out) {
    int seg = 0, i;
    memset(out, 0, 16);
    int double_colon = -1;
    for (i = 0; s[i]; i++) {
        if (s[i] == ':' && s[i+1] == ':') { double_colon = i; break; }
    }
    if (double_colon >= 0) {
        int after = 0;
        for (i = double_colon + 2; s[i]; ) {
            if (s[i] == ':') { after++; i++; }
            else { while (s[i] && s[i] != ':') i++; }
        }
        if (s[double_colon + 2]) after++;
        int before = 0;
        for (i = 0; i < double_colon; ) {
            if (s[i] == ':') { before++; i++; }
            else { while (s[i] && s[i] != ':') i++; }
        }
        if (double_colon > 0 && s[0] != ':') before++;
        int written = 0;
        const char* p = s;
        while (*p && written < 8) {
            if (*p == ':' && *(p+1) == ':') {
                written += 8 - before - after;
                p += 2;
                continue;
            }
            if (*p == ':') { p++; continue; }
            unsigned short val = 0;
            while (*p && *p != ':') {
                val = (val << 4) | parse_hex_nibble(*p);
                p++;
            }
            out[written * 2] = (val >> 8) & 0xFF;
            out[written * 2 + 1] = val & 0xFF;
            written++;
        }
    } else {
        while (*s && seg < 16) {
            if (*s == ':') { seg++; s++; continue; }
            out[seg] = (out[seg] << 4) | parse_hex_nibble(*s);
            s++;
        }
    }
}

static int parse_ipv4(const char* s, unsigned int* out) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int n = 0;
    while (*s && n < 4) {
        if (*s >= '0' && *s <= '9') {
            if (n == 0) a = a * 10 + (*s - '0');
            else if (n == 1) b = b * 10 + (*s - '0');
            else if (n == 2) c = c * 10 + (*s - '0');
            else if (n == 3) d = d * 10 + (*s - '0');
        } else if (*s == '.') {
            n++;
        }
        s++;
    }
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

int main(int argc, char** argv) {
    const char* addr_str;
    unsigned short port;
    int af;
    int do_bind = 0;

    if (argc < 2) {
        addr_str = "fe80::5054:ff:fe12:3456";
        port = 9999;
    } else {
        addr_str = argv[1];
        port = (argc > 2) ? (unsigned short)atoi(argv[2]) : 9999;
        if (argc > 3 && strcmp(argv[3], "--bind") == 0) do_bind = 1;
    }
    af = (strchr(addr_str, ':') != NULL) ? AF_INET6 : AF_INET;

    printf("[UDP] socket(%s, SOCK_DGRAM)... ", af == AF_INET6 ? "AF_INET6" : "AF_INET");
    int fd = socket(af, SOCK_DGRAM, 0);
    if (fd < 0) { printf("FAIL: errno=%d\n", errno); return 1; }
    printf("fd=%d\n", fd);

    if (do_bind) {
        if (af == AF_INET6) {
            struct sockaddr_in6 any;
            memset(&any, 0, sizeof(any));
            any.sin6_family = AF_INET6;
            printf("[UDP] bind(ephemeral)... ");
            if (bind(fd, (struct sockaddr*)&any, sizeof(any)) < 0) {
                printf("FAIL: errno=%d\n", errno); close(fd); return 1;
            }
        } else {
            struct sockaddr_in any;
            memset(&any, 0, sizeof(any));
            any.sin_family = AF_INET;
            printf("[UDP] bind(ephemeral)... ");
            if (bind(fd, (struct sockaddr*)&any, sizeof(any)) < 0) {
                printf("FAIL: errno=%d\n", errno); close(fd); return 1;
            }
        }
        printf("OK\n");
    }

    /* Test setsockopt(SO_RCVTIMEO) */
    printf("[UDP] setsockopt(SO_RCVTIMEO, 0)... ");
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (sr < 0) { printf("FAIL: errno=%d\n", errno); close(fd); return 1; }
    printf("OK\n");

    struct sockaddr_in6 addr6;
    struct sockaddr_in addr4;
    struct sockaddr* sa;
    socklen_t sl;

    if (af == AF_INET6) {
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = __builtin_bswap16(port);
        parse_ipv6(addr_str, addr6.sin6_addr);
        sa = (struct sockaddr*)&addr6;
        sl = sizeof(addr6);
    } else {
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = __builtin_bswap16(port);
        parse_ipv4(addr_str, &addr4.sin_addr);
        sa = (struct sockaddr*)&addr4;
        sl = sizeof(addr4);
    }

    const char* msg = "hello from guest via UDP";
    printf("[UDP] sendto(%s, %u)... ", addr_str, port);
    printf("[DBG] A\n");
    ssize_t n = sendto(fd, msg, strlen(msg), 0, sa, sl);
    if (n < 0) { printf("FAIL: errno=%d\n", errno); close(fd); return 1; }
    printf("sent %ld bytes\n", (long)n);

    char buf[128];
    unsigned char reply_raw[sizeof(struct sockaddr_in6)];
    struct sockaddr* reply_sa = (struct sockaddr*)reply_raw;
    socklen_t reply_sl = sizeof(reply_raw);
    printf("[UDP] recvfrom... ");
    n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reply_sa, &reply_sl);
    if (n < 0) { printf("FAIL: errno=%d\n", errno); close(fd); return 1; }
    buf[n] = '\0';
    printf("got %ld bytes: \"%s\"\n", (long)n, buf);

    printf("[UDP] close... ");
    close(fd);
    printf("OK\n");

    printf("[UDP] PASS\n");
    return 0;
}
