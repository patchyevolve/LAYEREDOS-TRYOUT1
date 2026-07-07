#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define CLONE_VM        0x100
#define CLONE_THREAD    0x20000

static int pipe_fds[2];
static int thread_pipe[2];
static int global_counter = 0;

static int pipe_test(void) {
    write(STDOUT_FILENO, "--- pipe_test ---\n", 19);
    int fds[2];
    if (pipe(fds) < 0) {
        write(STDOUT_FILENO, "FAIL: pipe create\n", 19);
        return 1;
    }
    const char* msg = "pipe data";
    write(fds[1], msg, 10);
    char buf[32];
    int n = read(fds[0], buf, sizeof(buf));
    if (n != 10 || strncmp(buf, "pipe data", 10) != 0) {
        write(STDOUT_FILENO, "FAIL: pipe read/write\n", 22);
        return 1;
    }
    close(fds[0]);
    close(fds[1]);
    write(STDOUT_FILENO, "pipe_test: PASS\n", 17);
    return 0;
}

static int thread_worker(void* arg) {
    write(STDOUT_FILENO, "  [thread] started\n", 20);
    global_counter++;
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "  [thread] writing to pipe, counter=%d\n", global_counter);
    write(thread_pipe[1], msg, n);
    write(STDOUT_FILENO, "  [thread] exiting\n", 20);
    return 42;
}

static int fork_test(void) {
    write(STDOUT_FILENO, "--- fork_test ---\n", 19);
    if (pipe(pipe_fds) < 0) {
        write(STDOUT_FILENO, "FAIL: pipe\n", 12);
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        write(STDOUT_FILENO, "FAIL: fork\n", 12);
        return 1;
    }
    if (pid == 0) {
        const char* msg = "hello from child";
        write(pipe_fds[1], msg, 16);
        close(pipe_fds[1]);
        _exit(0);
    }
    char buf[32];
    int n = read(pipe_fds[0], buf, sizeof(buf));
    close(pipe_fds[0]);
    if (n != 16 || strncmp(buf, "hello from child", 16) != 0) {
        write(STDOUT_FILENO, "FAIL: fork pipe data mismatch\n", 31);
        return 1;
    }
    int status;
    pid_t ret = waitpid(pid, &status, 0);
    if (ret != pid) {
        write(STDOUT_FILENO, "FAIL: waitpid\n", 15);
        return 1;
    }
    write(STDOUT_FILENO, "fork_test: PASS\n", 17);
    return 0;
}

static int mmap_test(void) {
    write(STDOUT_FILENO, "--- mmap_test ---\n", 19);
    size_t sz = 4096;
    void* p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        write(STDOUT_FILENO, "FAIL: mmap\n", 12);
        return 1;
    }
    unsigned char* cp = (unsigned char*)p;
    for (size_t i = 0; i < sz; i++) cp[i] = (unsigned char)(i & 0xFF);
    for (size_t i = 0; i < sz; i++) {
        if (cp[i] != (unsigned char)(i & 0xFF)) {
            write(STDOUT_FILENO, "FAIL: mmap data mismatch\n", 26);
            return 1;
        }
    }
    if (munmap(p, sz) < 0) {
        write(STDOUT_FILENO, "FAIL: munmap\n", 14);
        return 1;
    }
    write(STDOUT_FILENO, "mmap_test: PASS\n", 17);
    return 0;
}

static int mprotect_test(void) {
    write(STDOUT_FILENO, "--- mprotect_test ---\n", 23);
    void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        write(STDOUT_FILENO, "FAIL: mmap\n", 12);
        return 1;
    }
    if (mprotect(p, 4096, PROT_READ) < 0) {
        write(STDOUT_FILENO, "FAIL: mprotect\n", 16);
        munmap(p, 4096);
        return 1;
    }
    munmap(p, 4096);
    write(STDOUT_FILENO, "mprotect_test: PASS\n", 21);
    return 0;
}

static int fcntl_test(void) {
    write(STDOUT_FILENO, "--- fcntl_test ---\n", 20);
    int fds[2];
    if (pipe(fds) < 0) {
        write(STDOUT_FILENO, "FAIL: pipe\n", 12);
        return 1;
    }
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0) {
        write(STDOUT_FILENO, "FAIL: F_GETFL\n", 15);
        return 1;
    }
    if (fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        write(STDOUT_FILENO, "FAIL: F_SETFL\n", 15);
        return 1;
    }
    int newfd = fcntl(fds[0], F_DUPFD, 10);
    if (newfd < 10) {
        write(STDOUT_FILENO, "FAIL: F_DUPFD\n", 15);
        return 1;
    }
    close(newfd);
    close(fds[0]);
    close(fds[1]);
    write(STDOUT_FILENO, "fcntl_test: PASS\n", 18);
    return 0;
}

static int file_mmap_test(void) {
    write(STDOUT_FILENO, "--- file_mmap_test ---\n", 24);
    int fd = open("/hello-c.elf", 0);
    if (fd < 0) {
        write(STDOUT_FILENO, "FAIL: open /hello-c.elf\n", 25);
        return 1;
    }
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        write(STDOUT_FILENO, "FAIL: lseek end\n", 17);
        close(fd);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);
    size_t page_sz = (sz + 4095) & ~4095;
    void* p = mmap(NULL, page_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        write(STDOUT_FILENO, "FAIL: file mmap\n", 17);
        close(fd);
        return 1;
    }
    unsigned char* data = (unsigned char*)p;
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        write(STDOUT_FILENO, "FAIL: mmap ELF magic mismatch\n", 31);
        munmap(p, page_sz);
        close(fd);
        return 1;
    }
    if (munmap(p, page_sz) < 0) {
        write(STDOUT_FILENO, "FAIL: file munmap\n", 19);
        close(fd);
        return 1;
    }
    close(fd);
    write(STDOUT_FILENO, "file_mmap_test: PASS\n", 22);
    return 0;
}

static int clone_test(void) {
    write(STDOUT_FILENO, "--- clone_test ---\n", 20);
    if (pipe(thread_pipe) < 0) {
        write(STDOUT_FILENO, "FAIL: pipe for clone\n", 22);
        return 1;
    }
    void* stack = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        write(STDOUT_FILENO, "FAIL: mmap stack\n", 18);
        return 1;
    }
    void* stack_top = (void*)((unsigned long)stack + 8192);
    pid_t tid = clone(thread_worker, stack_top, CLONE_VM, NULL);
    if (tid < 0) {
        write(STDOUT_FILENO, "FAIL: clone\n", 13);
        munmap(stack, 8192);
        return 1;
    }
    write(STDOUT_FILENO, "  [main] clone returned, waiting for pipe data\n", 49);
    char buf[128];
    int n = read(thread_pipe[0], buf, sizeof(buf));
    if (n <= 0) {
        write(STDOUT_FILENO, "FAIL: no data from thread\n", 27);
        close(thread_pipe[0]);
        return 1;
    }
    buf[n] = '\0';
    write(STDOUT_FILENO, "  [main] received: ", 20);
    write(STDOUT_FILENO, buf, n);
    write(STDOUT_FILENO, "\n", 1);
    close(thread_pipe[0]);
    if (global_counter != 1) {
        write(STDOUT_FILENO, "FAIL: counter not incremented by thread\n", 41);
        return 1;
    }
    write(STDOUT_FILENO, "clone_test: PASS\n", 18);
    return 0;
}

static int dup2_test(void) {
    write(STDOUT_FILENO, "--- dup2_test ---\n", 19);
    int fds[2];
    if (pipe(fds) < 0) return 1;
    if (dup2(fds[1], 100) < 0) {
        write(STDOUT_FILENO, "FAIL: dup2\n", 12);
        return 1;
    }
    const char* msg = "dup2 test";
    write(100, msg, 9);
    char buf[32];
    int n = read(fds[0], buf, sizeof(buf));
    if (n != 9 || strncmp(buf, "dup2 test", 9) != 0) {
        write(STDOUT_FILENO, "FAIL: dup2 data mismatch\n", 26);
        return 1;
    }
    close(100);
    close(fds[0]);
    close(fds[1]);
    write(STDOUT_FILENO, "dup2_test: PASS\n", 17);
    return 0;
}

static int netns_test(void) {
    write(STDOUT_FILENO, "--- netns_test ---\n", 20);

    netconfig_req_t nr;
    memset(&nr, 0, sizeof(nr));
    int idx = veth_pair(&nr);
    if (idx < 0) {
        write(STDOUT_FILENO, "veth_pair: FAIL\n", 17);
        return 1;
    }
    write(STDOUT_FILENO, "veth_pair: PASS\n", 17);

    int ret = unshare(CLONE_NEWNET);
    if (ret < 0) {
        write(STDOUT_FILENO, "unshare: FAIL\n", 15);
        return 1;
    }
    write(STDOUT_FILENO, "unshare: PASS\n", 15);

    /* veth_pair should also work in the new namespace */
    memset(&nr, 0, sizeof(nr));
    int idx2 = veth_pair(&nr);
    if (idx2 < 0) {
        write(STDOUT_FILENO, "veth_pair(2): FAIL\n", 20);
        return 1;
    }
    write(STDOUT_FILENO, "veth_pair(2): PASS\n", 20);

    /* test netconfig GET_IPV4 in empty namespace */
    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_GET_IPV4;
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig GET_IPV4: FAIL\n", 26);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig GET_IPV4: PASS\n", 26);

    /* test netconfig SET_IPV4 */
    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_SET_IPV4;
    nr.addr4 = ipv4_from_bytes(192, 168, 99, 1);
    nr.prefix_len = 24;
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig SET_IPV4: FAIL\n", 26);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig SET_IPV4: PASS\n", 26);

    /* verify back */
    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_GET_IPV4;
    if (netconfig(&nr) < 0 || nr.addr4.bytes[2] != 99) {
        write(STDOUT_FILENO, "netconfig GET_IPV4 verify: FAIL\n", 33);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig GET_IPV4 verify: PASS\n", 33);

    /* test route_add/del */
    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_ADD_ROUTE_V4;
    nr.addr4 = ipv4_from_bytes(10, 0, 0, 0);
    nr.prefix_len = 8;
    nr.gw4 = ipv4_from_bytes(192, 168, 99, 2);
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig ADD_ROUTE: FAIL\n", 27);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig ADD_ROUTE: PASS\n", 27);

    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_DEL_ROUTE_V4;
    nr.addr4 = ipv4_from_bytes(10, 0, 0, 0);
    nr.prefix_len = 8;
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig DEL_ROUTE: FAIL\n", 27);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig DEL_ROUTE: PASS\n", 27);

    /* test arp set/del */
    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_SET_ARP;
    nr.addr4 = ipv4_from_bytes(192, 168, 99, 2);
    nr.mac[0] = 0x52; nr.mac[1] = 0x54; nr.mac[2] = 0x00;
    nr.mac[3] = 0x12; nr.mac[4] = 0x34; nr.mac[5] = 0x99;
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig SET_ARP: FAIL\n", 25);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig SET_ARP: PASS\n", 25);

    memset(&nr, 0, sizeof(nr));
    nr.op = NETCONFIG_DEL_ARP;
    nr.addr4 = ipv4_from_bytes(192, 168, 99, 2);
    if (netconfig(&nr) < 0) {
        write(STDOUT_FILENO, "netconfig DEL_ARP: FAIL\n", 25);
        return 1;
    }
    write(STDOUT_FILENO, "netconfig DEL_ARP: PASS\n", 25);

    write(STDOUT_FILENO, "netns_test: PASS\n", 18);
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    write(STDOUT_FILENO, "=== threading/libc syscall test ===\n", 36);
    int fail = 0;
    /* fork test must come first (before clone creates threads) */
    fail |= fork_test();
    fail |= pipe_test();
    fail |= dup2_test();
    fail |= mmap_test();
    fail |= mprotect_test();
    fail |= fcntl_test();
    fail |= clone_test();
    fail |= file_mmap_test();
    fail |= netns_test();
    if (fail)
        write(STDOUT_FILENO, "SOME TESTS FAILED\n", 19);
    else
        write(STDOUT_FILENO, "=== ALL TESTS PASSED ===\n", 26);
    return fail;
}
