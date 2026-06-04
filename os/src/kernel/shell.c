#include "kernel.h"
#include "shell.h"
#include "hal.h"
#include "pmm.h"
#include "sched.h"
#include "sync.h"
#include "vmm.h"
#include "eventbus.h"

#define SHELL_PROMPT "\nOS> "
#define SHELL_MAX_ARGS 16

static char line_buf[SHELL_LINE_BUF];
static int line_pos = 0;
static char history[SHELL_HISTORY][SHELL_LINE_BUF];
static int hist_count = 0;
static int hist_pos = 0;
static volatile uint64_t shell_start_tick = 0;

static void demo_task(void* arg) {
    int id = (int)(uint64_t)arg;
    kprintf("[Demo Task %d] Starting...\n", id);
    for (int i = 0; i < 3; i++) {
        kprintf("[Demo Task %d] Iteration %d/3, tick=%lu\n",
                id, i + 1, hal_timer_get_ticks());
        thread_yield();
    }
    kprintf("[Demo Task %d] Done.\n", id);
}

static void compute_task(void* arg) {
    int id = (int)(uint64_t)arg;
    uint64_t result = 0;
    for (int i = 0; i < 100000; i++) {
        result += i * (id + 1);
        if (i % 10000 == 0) thread_yield();
    }
    kprintf("[Compute %d] Result=%lu (tick=%lu)\n",
            id, result, hal_timer_get_ticks());
}

static void cmd_help(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Available commands:\n");
    kprintf("  help       - Show this help\n");
    kprintf("  echo <txt> - Print text\n");
    kprintf("  meminfo    - Show memory usage\n");
    kprintf("  ps         - Show thread list\n");
    kprintf("  top        - Show system stats\n");
    kprintf("  clear      - Clear screen\n");
    kprintf("  reboot     - Reboot system\n");
    kprintf("  poweroff   - Power off\n");
    kprintf("  version    - Show version\n");
    kprintf("  panic      - Trigger kernel panic (test)\n");
    kprintf("  demo       - Run demo (create threads)\n");
    kprintf("  compute    - Run compute workload\n");
    kprintf("  fault      - Test fault injection\n");
    kprintf("  uptime     - Show system uptime\n");
    kprintf("  stats      - Show kernel statistics\n");
    kprintf("  mutex      - Mutex contention test\n");
    kprintf("  event      - Event bus stress test\n");
    kprintf("  cleanup    - Reap zombie threads\n");
}

static void cmd_echo(char** args, int argc) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) kputchar(' ');
        kputs(args[i]);
    }
    kputchar('\n');
}

static void cmd_meminfo(char** args, int argc) {
    (void)args; (void)argc;
    uint64_t total = pmm_total_pages();
    uint64_t free = pmm_free_pages_count();
    uint64_t used = pmm_used_pages();
    uint64_t total_mb = total * 4 / 1024;
    uint64_t free_mb = free * 4 / 1024;
    uint64_t used_mb = used * 4 / 1024;

    kprintf("Memory Usage:\n");
    kprintf("  Total: %lu MB (%lu pages)\n", total_mb, total);
    kprintf("  Used:  %lu MB (%lu pages) %lu%%\n",
            used_mb, used, total ? used * 100 / total : 0);
    kprintf("  Free:  %lu MB (%lu pages) %lu%%\n",
            free_mb, free, total ? free * 100 / total : 0);
}

struct ps_ctx { int first; };

static void ps_callback(thread_t* t, void* ctx) {
    (void)ctx;
    const char* state_str = "????";
    switch (t->state) {
        case THREAD_CREATED:   state_str = "CREAT"; break;
        case THREAD_READY:     state_str = "READY"; break;
        case THREAD_RUNNING:   state_str = "RUN  "; break;
        case THREAD_BLOCKED:   state_str = "BLOCK"; break;
        case THREAD_SLEEPING:  state_str = "SLEEP"; break;
        case THREAD_ZOMBIE:    state_str = "ZOMBI"; break;
        case THREAD_TERMINATED:state_str = "TERM "; break;
    }
    kprintf("  %lu %s %3d   %s\n", t->id, state_str, t->priority, t->name);
}

static void cmd_ps(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Thread List (current=%lu):\n", current_thread ? current_thread->id : 0);
    kprintf("  ID    STATE    PRIORITY  NAME\n");
    sched_foreach(ps_callback, NULL);
}

static void cmd_top(char** args, int argc) {
    (void)args; (void)argc;
    uint64_t ticks = hal_timer_get_ticks();
    uint64_t free = pmm_free_pages_count();
    uint64_t total = pmm_total_pages();
    uint64_t uptime_s = ticks / 1000;

    kprintf("=== System Top ===\n");
    kprintf("Uptime:  %lus\n", uptime_s);
    kprintf("Ticks:   %lu\n", ticks);
    kprintf("Memory:  %lu%% free (%lu/%lu MB)\n",
            total ? free * 100 / total : 0,
            (total - free) * 4 / 1024, total * 4 / 1024);
    kprintf("Thread:  %lu (current)\n", current_thread ? current_thread->id : 0);
    kprintf("State:   %d\n", current_thread ? (int)current_thread->state : -1);
    kprintf("Priority:%d\n", current_thread ? current_thread->priority : 0);
}

static void cmd_clear(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("\033[2J\033[H");
}

static void cmd_version(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("OPERtur/TRY1 OS v0.1.0\n");
    kprintf("Layered x86-64 Kernel\n");
    kprintf("Architecture: 9-layer model\n");
    kprintf("Built: %s %s\n", __DATE__, __TIME__);
}

static void cmd_reboot(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Rebooting...\n");
    hal_reboot();
}

static void cmd_poweroff(char** args, int argc) {
    (void)args; (void)argc;
    sched_reap_zombies();
    kprintf("Threads remaining: %u\n", sched_thread_count());
    hal_poweroff();
}

static void cmd_panic(char** args, int argc) {
    (void)args; (void)argc;
    kpanic("User-triggered panic for testing");
}

static void cmd_demo(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Starting demo: creating 3 tasks...\n");
    thread_t* t1 = thread_create(demo_task, (void*)1, THREAD_DEF_PRIO, "demo1");
    thread_t* t2 = thread_create(demo_task, (void*)2, THREAD_DEF_PRIO, "demo2");
    thread_t* t3 = thread_create(demo_task, (void*)3, THREAD_DEF_PRIO, "demo3");

    if (!t1 || !t2 || !t3) {
        kprintf("Failed to create demo threads!\n");
        return;
    }

    sched_add_thread(t1);
    sched_add_thread(t2);
    sched_add_thread(t3);

    kprintf("Demo threads created and scheduled.\n");
}

static void cmd_compute(char** args, int argc) {
    (void)args; (void)argc;
    int n = 3;
    if (argc > 1) {
        n = 0;
        for (char* p = args[1]; *p; p++) n = n * 10 + (*p - '0');
        if (n < 1) n = 1;
        if (n > 50) n = 50;
    }

    kprintf("Spawning %d compute tasks...\n", n);
    for (int i = 0; i < n; i++) {
        thread_t* t = thread_create(compute_task, (void*)(uint64_t)i,
                                    THREAD_DEF_PRIO, "compute");
        if (t) sched_add_thread(t);
    }
}

static void cmd_stats(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("=== Kernel Statistics ===\n");
    kprintf("Scheduler: %s\n", sched_running ? "RUNNING" : "STOPPED");
    kprintf("Timer ticks: %lu\n", hal_timer_get_ticks());
    kprintf("Physical pages: %lu total, %lu free\n",
            pmm_total_pages(), pmm_free_pages_count());

    if (current_thread) {
        kprintf("Current thread: %lu (%s) prio=%d ticks=%lu\n",
                current_thread->id, current_thread->name,
                current_thread->priority, current_thread->total_ticks);
    }
}

static void cmd_uptime(char** args, int argc) {
    (void)args; (void)argc;
    uint64_t ticks = hal_timer_get_ticks();
    uint64_t seconds = ticks / 1000;
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;

    kprintf("Uptime: %luh %lum %lus (%lu ticks)\n",
            hours, minutes % 60, seconds % 60, ticks);
}

static mutex_t shared_mutex;
static volatile uint64_t shared_counter;
static int mutex_test_done;

static void mutex_worker(void* arg) {
    int id = (int)(uint64_t)arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&shared_mutex, (uint64_t)-1);
        uint64_t val = shared_counter;
        thread_yield();
        shared_counter = val + 1;
        mutex_unlock(&shared_mutex);
        thread_yield();
    }
    kprintf("[Mutex %d] done (final counter would be %lu)\n", id, shared_counter);
}

static void cmd_mutex(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Mutex contention test: 5 threads, 50 iterations each\n");
    mutex_init(&shared_mutex);
    shared_counter = 0;
    mutex_test_done = 0;
    for (int i = 0; i < 5; i++) {
        thread_t* t = thread_create(mutex_worker, (void*)(uint64_t)(i + 1),
                                    THREAD_DEF_PRIO, "mutex");
        if (t) sched_add_thread(t);
    }
    kprintf("Spawned 5 mutex contention threads.\n");
}

static void cmd_event(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Event bus stress test: publishing 100 events...\n");
    for (int i = 0; i < 100; i++) {
        eventbus_publish(EV_USER_EVENT, (uint64_t)i, 0, 0, 0);
        thread_yield();
    }
    kprintf("Published 100 events. Total: %lu\n", eventbus_count());
}

static void cmd_cleanup(char** args, int argc) {
    (void)args; (void)argc;
    uint32_t before = sched_thread_count();
    sched_reap_zombies();
    uint32_t after = sched_thread_count();
    kprintf("Threads: %u before, %u after reaping\n", before, after);
}

static void cmd_fault(char** args, int argc) {
    (void)args; (void)argc;
    kprintf("Fault injection test:\n");
    kprintf("  1. Allocate until OOM\n");
    kprintf("  2. NULL pointer dereference (will panic)\n");
    kprintf("Select test (1-2): ");

    char c = hal_uart_getchar();
    kprintf("%c\n", c);

    if (c == '1') {
        kprintf("Allocating until OOM...\n");
        while (1) {
            uint64_t page = pmm_alloc_page();
            if (!page) {
                kprintf("OOM reached! Freed all pages.\n");
                break;
            }
        }
    } else if (c == '2') {
        kprintf("Triggering NULL dereference...\n");
        volatile int* p = NULL;
        *p = 42;
    } else {
        kprintf("Invalid test.\n");
    }
}

typedef struct {
    const char* name;
    void (*func)(char**, int);
    const char* desc;
} shell_cmd_t;

static shell_cmd_t commands[] = {
    {"help",     cmd_help,     "Show this help"},
    {"echo",     cmd_echo,     "Print text"},
    {"meminfo",  cmd_meminfo,  "Show memory usage"},
    {"ps",       cmd_ps,       "Show thread list"},
    {"top",      cmd_top,      "Show system stats"},
    {"clear",    cmd_clear,    "Clear screen"},
    {"version",  cmd_version,  "Show version"},
    {"reboot",   cmd_reboot,   "Reboot system"},
    {"poweroff", cmd_poweroff, "Power off"},
    {"panic",    cmd_panic,    "Trigger kernel panic"},
    {"demo",     cmd_demo,     "Run demo with threads"},
    {"compute",  cmd_compute,  "Run compute workload"},
    {"stats",    cmd_stats,    "Show kernel statistics"},
    {"uptime",   cmd_uptime,   "Show system uptime"},
    {"fault",    cmd_fault,    "Test fault injection"},
    {"mutex",    cmd_mutex,    "Mutex contention test"},
    {"event",    cmd_event,    "Event bus stress test"},
    {"cleanup",  cmd_cleanup,  "Reap zombie threads"},
    {NULL, NULL, NULL}
};

static void process_line(const char* line) {
    char* args[SHELL_MAX_ARGS];
    int argc = 0;

    char buf[SHELL_LINE_BUF];
    kstrncpy(buf, line, SHELL_LINE_BUF - 1);

    char* p = buf;
    while (*p && argc < SHELL_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        args[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc == 0) return;

    for (int i = 0; commands[i].name; i++) {
        if (kstrcmp(commands[i].name, args[0]) == 0) {
            commands[i].func(args, argc);
            return;
        }
    }

    kprintf("Unknown command: %s\n", args[0]);
}

static void add_history(const char* line) {
    if (!*line) return;
    int idx = hist_count % SHELL_HISTORY;
    kstrncpy(history[idx], line, SHELL_LINE_BUF - 1);
    hist_count++;
    hist_pos = hist_count;
}

void shell_init(void) {
    kprintf("[SHELL] Layer N-1 initialized\n");
    shell_start_tick = hal_timer_get_ticks();
}

void shell_run(void) {
    kprintf("\n");
    kprintf("╔══════════════════════════════════════╗\n");
    kprintf("║  OPERtur/TRY1 OS v0.1.0              ║\n");
    kprintf("║  Layered x86-64 Kernel               ║\n");
    kprintf("║  Type 'help' for commands             ║\n");
    kprintf("╚══════════════════════════════════════╝\n");

    line_pos = 0;
    int hist_idx = hist_count - 1;
    if (hist_idx < 0) hist_idx = 0;

    kprintf(SHELL_PROMPT);

    for (;;) {
        eventbus_dispatch();
        if (need_reschedule) schedule();
        if (!hal_uart_data_available()) {
            thread_yield();
            if (!hal_uart_data_available()) {
                asm volatile("pause");
                continue;
            }
        }
        char c = hal_uart_getchar();

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                add_history(line_buf);
                process_line(line_buf);
                line_pos = 0;
            }
            kprintf(SHELL_PROMPT);
        } else if (c == '\b' || c == 127) {
            if (line_pos > 0) {
                line_pos--;
                kprintf("\b \b");
            }
        } else if (c >= ' ' && c <= '~') {
            if (line_pos < SHELL_LINE_BUF - 1) {
                line_buf[line_pos++] = c;
                kputchar(c);
            }
        }
    }
}
