#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define ERR_AGAIN -7

static int expect(int cond, const char* msg) {
    if (!cond) { write(2, "FAIL: ", 6); write(2, msg, strlen(msg)); write(2, "\n", 1); return 1; }
    write(2, "PASS: ", 6); write(2, msg, strlen(msg)); write(2, "\n", 1);
    return 0;
}

int main(void) {
    int failures = 0;
    write(1, "=== netns_config_test ===\n", 27);

    /* ── Step 1: Create veth pair ── */
    netconfig_req_t req;
    memset(&req, 0, sizeof(req));
    int idx = veth_pair(&req);
    if (idx < 0) { write(1, "veth_pair: FAIL\n", 17); return 1; }
    int veth_idx = idx;
    unsigned char mac_a[6], mac_b[6];
    memcpy(mac_a, req.mac, 6);
    memcpy(mac_b, req.addr6, 6);

    /* ── Step 2: Fork — child sets up separate namespace ── */
    int pid = fork();
    if (pid < 0) { write(1, "fork: FAIL\n", 12); return 1; }

    if (pid == 0) {
        /* ── Child: create new namespace ── */
        write(1, "[child] unshare(CLONE_NEWNET)...\n", 33);
        if (unshare(0x40000000) < 0) { write(1, "[child] unshare: FAIL\n", 23); return 1; }

        /* Move veth end A into our namespace */
        write(1, "[child] veth_move...\n", 22);
        if (veth_move(veth_idx, 0) < 0) { write(1, "[child] veth_move: FAIL\n", 25); return 1; }

        /* Configure our IP */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_SET_IPV4;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 2);
        req.prefix_len = 24;
        if (netconfig(&req) < 0) { write(1, "[child] set_ipv4: FAIL\n", 23); return 1; }

        /* Add ARP for peer */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_SET_ARP;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 1);
        memcpy(req.mac, mac_b, 6);
        if (netconfig(&req) < 0) { write(1, "[child] set_arp: FAIL\n", 22); return 1; }

        /* Add route to peer subnet */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_ADD_ROUTE_V4;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 0);
        req.prefix_len = 24;
        req.gw4 = ipv4_from_bytes(0, 0, 0, 0);
        if (netconfig(&req) < 0) { write(1, "[child] add_route: FAIL\n", 24); return 1; }

        /* Create TCP listener socket on port 9999 */
        int sfd = socket(2, 1, 0); /* AF_INET, SOCK_STREAM */
        if (sfd < 0) { write(1, "[child] socket: FAIL\n", 22); return 1; }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = 2;
        saddr.sin_port = 0x0F27; /* 9999 in network byte order */
        saddr.sin_addr = 0x01C80A; /* 10.200.1.2 in network byte order */

        if (bind(sfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0)
            { write(1, "[child] bind: FAIL\n", 20); return 1; }
        if (listen(sfd, 1) < 0)
            { write(1, "[child] listen: FAIL\n", 22); return 1; }
        write(1, "[child] listening on 10.200.1.2:9999\n", 38);

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) { write(1, "[child] accept: FAIL\n", 21); return 1; }
        write(1, "[child] accepted connection\n", 29);

        char buf[64];
        int n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            write(1, "[child] recv: ", 15);
            write(1, buf, n);
            write(1, "\n", 1);
            send(cfd, "pong", 4, 0);
        }

        close(cfd);
        close(sfd);
        write(1, "[child] done\n", 14);
        return 0;
    } else {
        /* ── Parent: configure our side ── */
        write(1, "[parent] configuring...\n", 24);

        /* Set our IP */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_SET_IPV4;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 1);
        req.prefix_len = 24;
        if (netconfig(&req) < 0) { write(1, "[parent] set_ipv4: FAIL\n", 24); failures++; }

        /* Add ARP for child's veth end */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_SET_ARP;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 2);
        memcpy(req.mac, mac_a, 6);
        if (netconfig(&req) < 0) { write(1, "[parent] set_arp: FAIL\n", 23); failures++; }

        /* Add route to child subnet */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_ADD_ROUTE_V4;
        req.addr4 = ipv4_from_bytes(10, 200, 1, 0);
        req.prefix_len = 24;
        req.gw4 = ipv4_from_bytes(0, 0, 0, 0);
        if (netconfig(&req) < 0) { write(1, "[parent] add_route: FAIL\n", 25); failures++; }

        /* Verify GET_IPV4 */
        memset(&req, 0, sizeof(req));
        req.op = NETCONFIG_GET_IPV4;
        if (netconfig(&req) < 0) { write(1, "[parent] get_ipv4: FAIL\n", 24); failures++; }
        if (req.addr4.bytes[0] != 10 || req.addr4.bytes[1] != 200 ||
            req.addr4.bytes[2] != 1 || req.addr4.bytes[3] != 1)
            { write(1, "[parent] get_ipv4 value: FAIL\n", 31); failures++; }

        /* Give child time to set up */
        sleep(1);

        /* ── Parent: initiate TCP connect to child ── */
        write(1, "[parent] connecting to 10.200.1.2:9999...\n", 42);
        int sfd = socket(2, 1, 0);
        if (sfd < 0) { write(1, "[parent] socket: FAIL\n", 23); failures++; }

        struct sockaddr_in daddr;
        memset(&daddr, 0, sizeof(daddr));
        daddr.sin_family = 2;
        daddr.sin_port = 0x0F27; /* 9999 */
        daddr.sin_addr = 0x02C80A; /* 10.200.1.2 in network byte order */

        if (connect(sfd, (struct sockaddr*)&daddr, sizeof(daddr)) < 0) {
            write(1, "[parent] connect: FAIL\n", 24);
            failures++;
        } else {
            write(1, "[parent] connected, sending ping...\n", 37);
            send(sfd, "ping", 4, 0);

            char buf[64];
            int n = recv(sfd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                write(1, "[parent] recv: ", 16);
                write(1, buf, n);
                write(1, "\n", 1);
                failures += expect(n == 4 && memcmp(buf, "pong", 4) == 0,
                                    "cross-ns TCP echo");
            } else {
                write(1, "[parent] recv: FAIL (empty)\n", 29);
                failures++;
            }
            close(sfd);
        }

        /* Wait for child */
        int wstatus;
        waitpid(pid, &wstatus, 0);
        write(1, "[parent] child exited\n", 22);

        failures += expect(wstatus == 0, "child exit status");

        if (failures == 0)
            write(1, "=== netns_config_test: ALL PASS ===\n", 37);
        else
            write(1, "=== netns_config_test: FAILURES ===\n", 37);
        return failures ? 1 : 0;
    }
}
