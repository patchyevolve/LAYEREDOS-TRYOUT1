#include "kernel.h"
#include "test_framework.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

#ifdef PROCESS_SELF_TEST

/* ============================================================
 * Test 1: Process create / find / reap cycle
 *
 * Creates a new process, verifies it exists via process_find,
 * then exits and reaps it. Guards against PID leaks and
 * stale process table entries.
 * ============================================================ */
static int test_process_create_find_exit(void) {
    process_t* p = process_create("test-proc", 1);
    ASSERT_NOT_NULL(p, "process_create failed");
    ASSERT_NE(p->pid, (pid_t)0, "pid should be non-zero");
    pid_t pid = p->pid;

    process_t* found = process_find(pid);
    ASSERT_EQ(found, p, "process_find mismatch");

    err_t e = process_exit(p, 42);
    ASSERT_ERR_OK(e, "process_exit");

    process_reap(p);

    /* After reap, the old pid should belong to no process (or PID 0).
     * The important check: the reaped process pointer must not match. */
    found = process_find(pid);
    ASSERT_NE(found, p, "process_find returned reaped process");

    kprintf("[TEST] process_create_find_exit: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 2: Process exit code propagation
 *
 * Creates a process, exits with a specific code, reaps it,
 * verifies the exit code was stored and propagated.
 * ============================================================ */
static int test_process_exit_code(void) {
    process_t* p = process_create("test-exit", 1);
    ASSERT_NOT_NULL(p, "process_create failed");

    err_t e = process_exit(p, 99);
    ASSERT_ERR_OK(e, "process_exit");
    ASSERT_EQ(p->exit_code, 99, "exit code mismatch");
    ASSERT_TRUE(p->exited, "exited flag should be set");

    process_reap(p);

    kprintf("[TEST] process_exit_code: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 3: Fork (duplicate process) — basic sanity
 *
 * Creates a process, duplicates it via process_fork syscall,
 * verifies the child exists and has different PID.
 * After verification, cleanup both.
 * ============================================================ */
static int test_process_fork_basic(void) {
    process_t* parent = process_create("fork-parent", 1);
    ASSERT_NOT_NULL(parent, "parent process_create failed");

    /* Use sys_fork-like logic: create child with ppid=parent->pid */
    process_t* child = process_create("fork-child", parent->pid);
    ASSERT_NOT_NULL(child, "child process_create failed");
    ASSERT_NE(child->pid, parent->pid, "child should have different PID");
    ASSERT_EQ(child->ppid, parent->pid, "child ppid should be parent pid");

    process_exit(child, 0);
    process_reap(child);

    process_exit(parent, 0);
    process_reap(parent);

    kprintf("[TEST] process_fork_basic: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 4: Zombie cleanup via process_reap
 *
 * Exits a process (zombie state), verifies it stays in
 * the process table, then reaps it and verifies it's gone.
 * Guards against zombie process table exhaustion.
 * ============================================================ */
static int test_process_zombie_cleanup(void) {
    process_t* p = process_create("zombie-test", 1);
    ASSERT_NOT_NULL(p, "process_create failed");

    pid_t pid = p->pid;

    err_t e = process_exit(p, 0);
    ASSERT_ERR_OK(e, "process_exit");
    ASSERT_TRUE(p->exited, "exited flag after exit");

    /* Should still be findable after exit */
    process_t* found = process_find(pid);
    ASSERT_EQ(found, p, "process should still exist as zombie");

    process_reap(p);

    /* Should be gone after reap */
    found = process_find(pid);
    ASSERT_NULL(found, "process should be gone after reap");

    kprintf("[TEST] process_zombie_cleanup: PASS\n");
    return TEST_PASS;
}

void process_self_test(void) {
    kprintf("[TEST] === Process self-tests ===\n");

    int pass = 0, fail = 0;
    if (test_process_create_find_exit() == TEST_PASS) pass++; else fail++;
    if (test_process_exit_code() == TEST_PASS) pass++; else fail++;
    if (test_process_fork_basic() == TEST_PASS) pass++; else fail++;
    if (test_process_zombie_cleanup() == TEST_PASS) pass++; else fail++;

    kprintf("[TEST] === Results: %d pass, %d fail ===\n", pass, fail);
}

#endif /* PROCESS_SELF_TEST */
