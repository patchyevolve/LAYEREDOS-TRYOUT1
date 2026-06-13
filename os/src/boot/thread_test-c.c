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
    if (fail)
        write(STDOUT_FILENO, "SOME TESTS FAILED\n", 19);
    else
        write(STDOUT_FILENO, "=== ALL TESTS PASSED ===\n", 26);
    return fail;
}
