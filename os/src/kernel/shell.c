/*
 * shell.c — Interactive kernel shell
 *
 * Features
 * ────────
 *  • Full readline-style line editing  (backspace, arrow keys, Home/End)
 *  • 64-entry circular command history  (↑/↓ navigation, `history` command)
 *  • Tab completion  — commands AND VFS paths (files + directories)
 *  • Pipeline support  —  cmd1 | cmd2 | cmd3  (up to SHELL_MAX_PIPE stages)
 *  • I/O redirection  —  > file,  >> file,  < file
 *  • Alias  —  alias, unalias
 *  • Shell environment variables  —  set, unset, printenv, $VAR expansion
 *  • Script / source  —  source <file>  (up to SHELL_SCRIPT_DEPTH deep)
 *  • Filesystem commands: ls, cat, cp, mv, touch, mkdir, rmdir, rm,
 *                         writefile, stat, pwd, cd, find, head, tail, wc
 *  • Process / scheduler: ps, top, demo, compute, mutex, event, cleanup,
 *                         kill, nice, run, usermode, elfload
 *  • System info: meminfo, uptime, stats, version, uname
 *  • Misc: echo, clear, help, reboot, poweroff, panic, fault,
 *          kbtest, atatest, format, mount
 */

#include "kernel.h"
#include "shell.h"
#include "hal.h"
#include "pmm.h"
#include "sched.h"
#include "sync.h"
#include "eventbus.h"
#include "vfs.h"
#include "ramdisk.h"
#include "keyboard.h"
#include "ata.h"
#include "process.h"
#include "elf.h"
#include "kmalloc.h"
#include "block.h"
#include "sfs.h"

extern char _binary_build_user_program_elf_start[];
extern char _binary_build_user_program_elf_end[];

/* ══════════════════════════════════════════════════════════════════════════
 * Internal state
 * ══════════════════════════════════════════════════════════════════════════ */

#define SHELL_PROMPT   "\nOS> "

/* Line editing */
static char line_buf[SHELL_LINE_BUF];
static int  line_pos  = 0;          /* cursor position (== length when at end) */
static int  line_len  = 0;          /* total chars in buffer                   */

/* History */
static char history[SHELL_HISTORY][SHELL_LINE_BUF];
static int  hist_count = 0;         /* total lines ever added (wraps mod)      */
static int  hist_nav   = 0;         /* navigation index during ↑/↓             */
static char hist_saved[SHELL_LINE_BUF]; /* line saved before navigating        */

/* Aliases */
static shell_alias_t aliases[SHELL_ALIAS_MAX];
static int           alias_count = 0;

/* Environment */
typedef struct { char name[SHELL_ENV_NAME]; char val[SHELL_ENV_VAL]; } env_var_t;
static env_var_t env_vars[SHELL_ENV_MAX];
static int       env_count = 0;

/* Current working directory */
static char cwd[SHELL_LINE_BUF] = "/";

/* Script nesting depth */
static int script_depth = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * Pipe buffer helpers
 * ══════════════════════════════════════════════════════════════════════════ */

void pipe_buf_write(pipe_buf_t* buf, const char* data, int len) {
    if (!buf) return;
    int space = SHELL_PIPE_BUF - buf->len;
    if (len > space) len = space;
    kmemcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

int pipe_buf_readc(pipe_buf_t* buf) {
    if (!buf || buf->pos >= buf->len) return -1;
    return (unsigned char)buf->data[buf->pos++];
}

/* Emit a string to out-buffer or console */
static void cmd_puts(pipe_buf_t* out, const char* s) {
    if (out) pipe_buf_write(out, s, kstrlen(s));
    else     kputs(s);
}

/* printf into a buffer or directly to console */
static void cmd_printf(pipe_buf_t* out, const char* fmt, ...) {
    char tmp[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    cmd_puts(out, tmp);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Environment variables
 * ══════════════════════════════════════════════════════════════════════════ */

void shell_setenv(const char* name, const char* value) {
    for (int i = 0; i < env_count; i++) {
        if (kstrcmp(env_vars[i].name, name) == 0) {
            kstrncpy(env_vars[i].val, value, SHELL_ENV_VAL - 1);
            return;
        }
    }
    if (env_count < SHELL_ENV_MAX) {
        kstrncpy(env_vars[env_count].name,  name,  SHELL_ENV_NAME - 1);
        kstrncpy(env_vars[env_count].val,   value, SHELL_ENV_VAL  - 1);
        env_count++;
    }
}

const char* shell_getenv(const char* name) {
    for (int i = 0; i < env_count; i++)
        if (kstrcmp(env_vars[i].name, name) == 0)
            return env_vars[i].val;
    return NULL;
}

void shell_unsetenv(const char* name) {
    for (int i = 0; i < env_count; i++) {
        if (kstrcmp(env_vars[i].name, name) == 0) {
            env_count--;
            kmemcpy(&env_vars[i], &env_vars[env_count], sizeof(env_var_t));
            return;
        }
    }
}

void shell_printenv(void) {
    for (int i = 0; i < env_count; i++)
        kprintf("  %s=%s\n", env_vars[i].name, env_vars[i].val);
}

/* ── $VAR expansion ──────────────────────────────────────────────────────── */
/* Expand $NAME in src into dst (dst_size includes NUL terminator).          */
static void expand_vars(const char* src, char* dst, int dst_size) {
    int d = 0;
    const char* s = src;
    while (*s && d < dst_size - 1) {
        if (*s == '$') {
            s++;
            char vname[SHELL_ENV_NAME];
            int  vn = 0;
            while (*s && ((*s >= 'a' && *s <= 'z') ||
                          (*s >= 'A' && *s <= 'Z') ||
                          (*s >= '0' && *s <= '9') ||
                          *s == '_') && vn < SHELL_ENV_NAME - 1)
                vname[vn++] = *s++;
            vname[vn] = '\0';
            const char* val = shell_getenv(vname);
            if (!val) val = "";
            while (*val && d < dst_size - 1) dst[d++] = *val++;
        } else {
            dst[d++] = *s++;
        }
    }
    dst[d] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════════
 * Aliases
 * ══════════════════════════════════════════════════════════════════════════ */

void shell_alias_set(const char* name, const char* value) {
    for (int i = 0; i < alias_count; i++) {
        if (kstrcmp(aliases[i].name, name) == 0) {
            kstrncpy(aliases[i].value, value, SHELL_ENV_VAL - 1);
            return;
        }
    }
    if (alias_count < SHELL_ALIAS_MAX) {
        kstrncpy(aliases[alias_count].name,  name,  SHELL_ENV_NAME - 1);
        kstrncpy(aliases[alias_count].value, value, SHELL_ENV_VAL  - 1);
        alias_count++;
    }
}

const char* shell_alias_get(const char* name) {
    for (int i = 0; i < alias_count; i++)
        if (kstrcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    return NULL;
}

void shell_alias_del(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (kstrcmp(aliases[i].name, name) == 0) {
            alias_count--;
            kmemcpy(&aliases[i], &aliases[alias_count], sizeof(shell_alias_t));
            return;
        }
    }
}

void shell_alias_list(void) {
    if (alias_count == 0) { kprintf("No aliases defined.\n"); return; }
    for (int i = 0; i < alias_count; i++)
        kprintf("  alias %s='%s'\n", aliases[i].name, aliases[i].value);
}

/* ══════════════════════════════════════════════════════════════════════════
 * History
 * ══════════════════════════════════════════════════════════════════════════ */

static void add_history(const char* line) {
    if (!line || !*line) return;
    if (hist_count > 0) {
        int last = (hist_count - 1) % SHELL_HISTORY;
        if (kstrcmp(history[last], line) == 0) return;
    }
    int idx = hist_count % SHELL_HISTORY;
    kstrncpy(history[idx], line, SHELL_LINE_BUF - 1);
    hist_count++;
    hist_nav = hist_count;
}

void shell_history_print(void) {
    int start = (hist_count > SHELL_HISTORY) ? hist_count - SHELL_HISTORY : 0;
    for (int i = start; i < hist_count; i++) {
        kprintf("  %3d  %s\n", i + 1, history[i % SHELL_HISTORY]);
    }
}

void shell_history_clear(void) {
    hist_count = 0;
    hist_nav   = 0;
    kprintf("History cleared.\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Path helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Resolve a possibly-relative path against cwd into dst. */
static void resolve_path(const char* path, char* dst, int dst_sz) {
    if (path[0] == '/') {
        kstrncpy(dst, path, dst_sz - 1);
        return;
    }
    int clen = kstrlen(cwd);
    int plen = kstrlen(path);
    if (clen + 1 + plen >= dst_sz) {
        kstrncpy(dst, path, dst_sz - 1);
        return;
    }
    kstrncpy(dst, cwd, dst_sz - 1);
    if (cwd[clen - 1] != '/') { dst[clen] = '/'; clen++; dst[clen] = '\0'; }
    kstrncat(dst, path, (size_t)(dst_sz - clen - 1));

    /* Collapse ".." segments (simple in-place) */
    char tmp[SHELL_LINE_BUF];
    int  ti = 0;
    const char* s = dst;
    tmp[ti++] = '/';
    s++;
    while (*s) {
        char seg[256]; int si = 0;
        while (*s && *s != '/') seg[si++] = *s++;
        seg[si] = '\0';
        if (*s == '/') s++;
        if (si == 0 || kstrcmp(seg, ".") == 0) continue;
        if (kstrcmp(seg, "..") == 0) {
            if (ti > 1) { ti--; while (ti > 1 && tmp[ti-1] != '/') ti--; }
            continue;
        }
        if (ti > 1) tmp[ti++] = '/';
        for (int k = 0; k < si && ti < SHELL_LINE_BUF - 1; k++) tmp[ti++] = seg[k];
    }
    if (ti == 0) { tmp[ti++] = '/'; }
    tmp[ti] = '\0';
    kstrncpy(dst, tmp, dst_sz - 1);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tokeniser + pipeline parser
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char*  argv[SHELL_MAX_ARGS];
    int    argc;
    char*  redir_in;
    char*  redir_out;
    int    redir_append;
} pipeline_stage_t;

static int parse_pipeline(char* line, pipeline_stage_t stages[]) {
    int  nstages = 0;
    char* seg = line;

    while (seg && nstages < SHELL_MAX_PIPE) {
        pipeline_stage_t* st = &stages[nstages++];
        kmemset(st, 0, sizeof(pipeline_stage_t));

        char* pipe_pos = NULL;
        int in_q = 0;
        for (char* p = seg; *p; p++) {
            if (*p == '"') in_q = !in_q;
            if (!in_q && *p == '|') { pipe_pos = p; break; }
        }
        char* next_seg = NULL;
        if (pipe_pos) { *pipe_pos = '\0'; next_seg = pipe_pos + 1; }

        char* p = seg;
        while (*p && st->argc < SHELL_MAX_ARGS - 1) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;

            if (*p == '>') {
                p++;
                st->redir_append = (*p == '>');
                if (st->redir_append) p++;
                while (*p == ' ') p++;
                st->redir_out = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) { *p = '\0'; p++; }
                continue;
            }
            if (*p == '<') {
                p++;
                while (*p == ' ') p++;
                st->redir_in = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) { *p = '\0'; p++; }
                continue;
            }

            if (*p == '"') {
                p++;
                st->argv[st->argc++] = p;
                while (*p && *p != '"') p++;
                if (*p) { *p = '\0'; p++; }
                continue;
            }

            st->argv[st->argc++] = p;
            while (*p && *p != ' ' && *p != '\t' &&
                   *p != '>' && *p != '<' && *p != '|') p++;
            if (*p == ' ' || *p == '\t') { *p = '\0'; p++; }
        }
        st->argv[st->argc] = NULL;
        seg = next_seg;
    }
    return nstages;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tab completion
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char* name;
    void (*func)(char**, int, pipe_buf_t*, pipe_buf_t*);
    const char* desc;
} shell_cmd_t;
static shell_cmd_t commands[];

static void do_tab_complete(void) {
    if (line_pos == 0) return;

    int word_start = line_pos;
    while (word_start > 0 && line_buf[word_start - 1] != ' ') word_start--;

    char partial[SHELL_LINE_BUF];
    int  plen = line_pos - word_start;
    kmemcpy(partial, line_buf + word_start, plen);
    partial[plen] = '\0';

    int is_cmd = 1;
    for (int i = 0; i < word_start; i++)
        if (line_buf[i] != ' ') { is_cmd = 0; break; }

    if (is_cmd) {
        int   matches[64]; int nm = 0;
        for (int i = 0; commands[i].name; i++) {
            if (kstrncmp(commands[i].name, partial, (size_t)plen) == 0) {
                if (nm < 64) matches[nm++] = i;
            }
        }
        int alias_matches[SHELL_ALIAS_MAX]; int nam = 0;
        for (int i = 0; i < alias_count; i++)
            if (kstrncmp(aliases[i].name, partial, (size_t)plen) == 0)
                alias_matches[nam++] = i;

        int total = nm + nam;
        if (total == 1) {
            const char* full = (nm == 1) ? commands[matches[0]].name
                                         : aliases[alias_matches[0]].name;
            const char* rest = full + plen;
            while (*rest) {
                if (line_pos < SHELL_LINE_BUF - 1) {
                    line_buf[line_pos++] = *rest;
                    line_len = line_pos;
                    kputchar(*rest);
                }
                rest++;
            }
            if (line_pos < SHELL_LINE_BUF - 1) {
                line_buf[line_pos++] = ' ';
                line_len = line_pos;
                kputchar(' ');
            }
        } else if (total > 1) {
            kprintf("\n");
            for (int i = 0; i < nm; i++)
                kprintf("  %s\n", commands[matches[i]].name);
            for (int i = 0; i < nam; i++)
                kprintf("  %s (alias)\n", aliases[alias_matches[i]].name);
            kprintf(SHELL_PROMPT);
            line_buf[line_pos] = '\0';
            kprintf("%s", line_buf);
        }
    } else {
        char dir_part[SHELL_LINE_BUF];
        char file_part[SHELL_LINE_BUF];
        kmemset(dir_part, 0, SHELL_LINE_BUF);
        kmemset(file_part, 0, SHELL_LINE_BUF);
        dir_part[0] = '/';
        int  slash = -1;
        for (int i = plen - 1; i >= 0; i--) {
            if (partial[i] == '/') { slash = i; break; }
        }
        if (slash >= 0) {
            kmemcpy(dir_part, partial, slash + 1);
            dir_part[slash + 1] = '\0';
            kstrncpy(file_part, partial + slash + 1, SHELL_LINE_BUF - 1);
        } else {
            kstrncpy(dir_part, cwd, SHELL_LINE_BUF - 1);
            kstrncpy(file_part, partial, SHELL_LINE_BUF - 1);
        }

        int fplen = kstrlen(file_part);
        char resolved[SHELL_LINE_BUF];
        resolve_path(dir_part, resolved, SHELL_LINE_BUF);

        vfs_node_t* dir = vfs_find(resolved);
        if (!dir || !dir->fs || !dir->fs->ops || !dir->fs->ops->readdir) return;

        char match_name[256] = "";  int n_match = 0;
        uint32_t idx = 0;
        while (1) {
            vfs_node_t* child = NULL;
            if (dir->fs->ops->readdir(dir, idx++, &child) != 0 || !child) break;
            if (kstrncmp(child->name, file_part, (size_t)fplen) == 0) {
                n_match++;
                if (n_match == 1) kstrncpy(match_name, child->name, 255);
                else {
                    if (n_match == 2) {
                        kprintf("\n  %s\n", match_name);
                    }
                    kprintf("  %s\n", child->name);
                }
            }
            kfree(child);
        }
        if (n_match == 1) {
            const char* rest = match_name + fplen;
            while (*rest) {
                if (line_pos < SHELL_LINE_BUF - 1) {
                    line_buf[line_pos++] = *rest;
                    line_len = line_pos;
                    kputchar(*rest);
                }
                rest++;
            }
        } else if (n_match > 1) {
            kprintf(SHELL_PROMPT);
            line_buf[line_pos] = '\0';
            kprintf("%s", line_buf);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helper tasks (used by demo / compute commands)
 * ══════════════════════════════════════════════════════════════════════════ */

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
    int      id     = (int)(uint64_t)arg;
    uint64_t result = 0;
    for (int i = 0; i < 100000; i++) {
        result += i * (id + 1);
        if (i % 10000 == 0) thread_yield();
    }
    kprintf("[Compute %d] Result=%lu (tick=%lu)\n",
            id, result, hal_timer_get_ticks());
}

static mutex_t           shared_mutex;
static volatile uint64_t shared_counter;

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
    kprintf("[Mutex %d] done (counter now %lu)\n", id, shared_counter);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Built-in command implementations
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── help ──────────────────────────────────────────────────────────────── */
static void cmd_help(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out,
        "Filesystem   : ls cd pwd cat cp mv rm rmdir mkdir touch stat\n"
        "               writefile head tail wc find grep\n"
        "Process      : ps top demo compute mutex event cleanup kill nice\n"
        "               run usermode elfload\n"
        "System       : meminfo uptime stats version uname clear reboot\n"
        "               poweroff panic fault kbtest atatest format mount\n"
        "Shell        : echo set unset printenv alias unalias history\n"
        "               source help\n"
        "Pipeline     : cmd | cmd   (up to %d stages)\n"
        "Redirection  : > file   >> file   < file\n"
        "Variables    : $NAME expansion in any argument\n",
        SHELL_MAX_PIPE);
}

/* ── echo ──────────────────────────────────────────────────────────────── */
static void cmd_echo(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    int newline = 1;
    int start   = 1;
    if (argc > 1 && kstrcmp(args[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) cmd_puts(out, " ");
        cmd_puts(out, args[i]);
    }
    if (newline) cmd_puts(out, "\n");
}

/* ── meminfo ───────────────────────────────────────────────────────────── */
static void cmd_meminfo(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    uint64_t total = pmm_total_pages();
    uint64_t free  = pmm_free_pages_count();
    uint64_t used  = pmm_used_pages();
    cmd_printf(out,
        "Memory Usage:\n"
        "  Total: %lu MB (%lu pages)\n"
        "  Used:  %lu MB (%lu pages) %lu%%\n"
        "  Free:  %lu MB (%lu pages) %lu%%\n",
        total * 4 / 1024, total,
        used  * 4 / 1024, used,  total ? used  * 100 / total : 0,
        free  * 4 / 1024, free,  total ? free  * 100 / total : 0);
}

/* ── ps ────────────────────────────────────────────────────────────────── */
static pipe_buf_t* ps_out_ptr;
static void ps_cb(thread_t* t, void* ctx) {
    (void)ctx;
    const char* st = "????";
    switch (t->state) {
        case THREAD_CREATED:    st = "CREAT"; break;
        case THREAD_READY:      st = "READY"; break;
        case THREAD_RUNNING:    st = "RUN  "; break;
        case THREAD_BLOCKED:    st = "BLOCK"; break;
        case THREAD_SLEEPING:   st = "SLEEP"; break;
        case THREAD_ZOMBIE:     st = "ZOMBI"; break;
        case THREAD_TERMINATED: st = "TERM "; break;
    }
    cmd_printf(ps_out_ptr, "  %-4lu %s  %3d  %s\n",
               t->id, st, t->priority, t->name);
}
static void cmd_ps(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out, "Thread List (current=%lu):\n",
               current_thread ? current_thread->id : 0);
    cmd_printf(out, "  ID   STATE  PRI  NAME\n");
    ps_out_ptr = out;
    sched_foreach(ps_cb, NULL);
}

/* ── top ───────────────────────────────────────────────────────────────── */
static void cmd_top(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    uint64_t ticks   = hal_timer_get_ticks();
    uint64_t free    = pmm_free_pages_count();
    uint64_t total   = pmm_total_pages();
    uint64_t uptime  = ticks / 1000;
    cmd_printf(out,
        "=== System Top ===\n"
        "Uptime  : %lus\n"
        "Ticks   : %lu\n"
        "Memory  : %lu%% free (%lu/%lu MB)\n"
        "Switches: %lu\n"
        "Yields  : %lu\n",
        uptime, ticks,
        total ? free * 100 / total : 0,
        (total - free) * 4 / 1024, total * 4 / 1024,
        sched_get_switch_count(),
        sched_get_yield_count());
    if (current_thread)
        cmd_printf(out, "Current : %lu (%s) prio=%d ticks=%lu\n",
                   current_thread->id, current_thread->name,
                   current_thread->priority, current_thread->total_ticks);
}

/* ── clear ─────────────────────────────────────────────────────────────── */
static void cmd_clear(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("\033[2J\033[H");
}

/* ── version ───────────────────────────────────────────────────────────── */
static void cmd_version(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out,
        "OPERtur/TRY1 OS v0.1.0\n"
        "Layered x86-64 Kernel\n"
        "Architecture: 9-layer model\n"
        "Built: %s %s\n", __DATE__, __TIME__);
}

/* ── uname ─────────────────────────────────────────────────────────────── */
static void cmd_uname(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    int all = (argc == 1);
    for (int i = 1; i < argc; i++)
        if (kstrcmp(args[i], "-a") == 0) { all = 1; break; }
    if (all || (argc > 1 && kstrcmp(args[1], "-s") == 0))
        cmd_puts(out, "OPERtur ");
    if (all || (argc > 1 && kstrcmp(args[1], "-r") == 0))
        cmd_puts(out, "0.1.0 ");
    if (all || (argc > 1 && kstrcmp(args[1], "-m") == 0))
        cmd_puts(out, "x86_64");
    cmd_puts(out, "\n");
}

/* ── reboot / poweroff / panic ─────────────────────────────────────────── */
static void cmd_reboot(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Rebooting...\n"); hal_reboot();
}
static void cmd_poweroff(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    sched_reap_zombies();
    kprintf("Threads: %u  Powering off...\n", sched_thread_count());
    hal_poweroff();
}
static void cmd_panic(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kpanic("User-triggered panic for testing");
}

/* ── uptime ────────────────────────────────────────────────────────────── */
static void cmd_uptime(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    uint64_t ticks   = hal_timer_get_ticks();
    uint64_t seconds = ticks / 1000;
    uint64_t minutes = seconds / 60;
    uint64_t hours   = minutes / 60;
    cmd_printf(out, "Uptime: %luh %lum %lus (%lu ticks)\n",
               hours, minutes % 60, seconds % 60, ticks);
}

/* ── stats ─────────────────────────────────────────────────────────────── */
static void cmd_stats(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out,
        "=== Kernel Statistics ===\n"
        "Scheduler    : %s\n"
        "Context swchs: %lu\n"
        "Yields       : %lu\n"
        "Timer ticks  : %lu\n"
        "Memory       : %lu/%lu pages free\n",
        sched_running ? "RUNNING" : "STOPPED",
        sched_get_switch_count(),
        sched_get_yield_count(),
        hal_timer_get_ticks(),
        pmm_free_pages_count(), pmm_total_pages());
    if (current_thread)
        cmd_printf(out, "Thread       : %lu (%s) prio=%d ticks=%lu\n",
                   current_thread->id, current_thread->name,
                   current_thread->priority, current_thread->total_ticks);
}

/* ── demo ──────────────────────────────────────────────────────────────── */
static void cmd_demo(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Starting demo: creating 3 tasks...\n");
    for (int i = 1; i <= 3; i++) {
        thread_t* t = thread_create(demo_task, (void*)(uint64_t)i,
                                    THREAD_DEF_PRIO, "demo");
        if (t) sched_add_thread(t);
    }
    kprintf("Demo threads scheduled.\n");
}

/* ── compute ───────────────────────────────────────────────────────────── */
static void cmd_compute(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
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

/* ── mutex ─────────────────────────────────────────────────────────────── */
static void cmd_mutex(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Mutex contention test: 5 threads\n");
    mutex_init(&shared_mutex);
    shared_counter = 0;
    for (int i = 0; i < 5; i++) {
        thread_t* t = thread_create(mutex_worker, (void*)(uint64_t)(i + 1),
                                    THREAD_DEF_PRIO, "mutex");
        if (t) sched_add_thread(t);
    }
}

/* ── event ─────────────────────────────────────────────────────────────── */
static void cmd_event(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Event bus stress test: publishing 100 events...\n");
    for (int i = 0; i < 100; i++) {
        eventbus_publish(EV_USER_EVENT, (uint64_t)i, 0, 0, 0);
        thread_yield();
    }
    kprintf("Published. Total events: %lu\n", eventbus_count());
}

/* ── cleanup ───────────────────────────────────────────────────────────── */
static void cmd_cleanup(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    uint32_t before = sched_thread_count();
    sched_reap_zombies();
    kprintf("Threads: %u → %u after reaping\n", before, sched_thread_count());
}

/* ── kill ──────────────────────────────────────────────────────────────── */
static void cmd_kill(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: kill [-signal] <pid>\n"); return; }
    int sig = SIGTERM;
    int arg_start = 1;
    if (args[1][0] == '-') {
        sig = 0;
        for (char* p = args[1] + 1; *p; p++) sig = sig * 10 + (*p - '0');
        arg_start = 2;
    }
    for (int i = arg_start; i < argc; i++) {
        uint64_t pid = 0;
        for (char* p = args[i]; *p; p++) pid = pid * 10 + (*p - '0');
        if (pid < 1 || pid > 255) { kprintf("kill: invalid pid %lu\n", pid); continue; }
        process_t* proc = process_find((pid_t)pid);
        if (!proc) { kprintf("kill: pid %lu not found\n", pid); continue; }
        signal_send((pid_t)pid, sig);
        signal_process(proc);
    }
}

/* ── bg, fg, jobs ─────────────────────────────────────────────────────── */
#define SHELL_MAX_JOBS 16
static struct {
    pid_t pid;
    char  cmd[64];
    int   stopped;
} shell_jobs[SHELL_MAX_JOBS];
static int shell_job_count = 0;

static void shell_remove_job(pid_t pid) {
    for (int i = 0; i < SHELL_MAX_JOBS; i++) {
        if (shell_jobs[i].pid == pid) {
            shell_jobs[i].pid = 0;
            shell_job_count--;
            return;
        }
    }
}

static void shell_update_job(pid_t pid) {
    process_t* proc = process_find(pid);
    if (!proc) { shell_remove_job(pid); return; }
    for (int i = 0; i < SHELL_MAX_JOBS; i++) {
        if (shell_jobs[i].pid == pid) {
            shell_jobs[i].stopped = (proc->flags & PROC_FLAG_STOPPED) ? 1 : 0;
            return;
        }
    }
}

static void cmd_bg(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: bg <pid>\n"); return; }
    uint64_t pid = 0;
    for (char* p = args[1]; *p; p++) pid = pid * 10 + (*p - '0');
    signal_send((pid_t)pid, SIGCONT);
    signal_process(process_find((pid_t)pid));
    shell_update_job((pid_t)pid);
    kprintf("[bg] job %lu continued\n", pid);
}

static void cmd_fg(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: fg <pid>\n"); return; }
    uint64_t pid = 0;
    for (char* p = args[1]; *p; p++) pid = pid * 10 + (*p - '0');
    process_t* proc = process_find((pid_t)pid);
    if (!proc) { kprintf("fg: pid %lu not found\n", pid); return; }
    /* Continue if stopped */
    if (proc->flags & PROC_FLAG_STOPPED) {
        signal_send((pid_t)pid, SIGCONT);
        signal_process(proc);
    }
    shell_update_job((pid_t)pid);
    /* Wait for the process to exit */
    while (!proc->exited && !(proc->flags & PROC_FLAG_STOPPED))
        sched_block(&proc->exit_waiters);
    if (proc->exited) {
        kprintf("[fg] pid %lu exited with code %d\n", pid, proc->exit_code);
        shell_remove_job((pid_t)pid);
    }
}

static void cmd_jobs(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc;
    int found = 0;
    for (int i = 0; i < SHELL_MAX_JOBS; i++) {
        if (shell_jobs[i].pid) {
            shell_update_job(shell_jobs[i].pid);
            if (shell_jobs[i].pid) {
                cmd_printf(out, "[%d]  %c  %s  (pid %d)\n", i + 1,
                    shell_jobs[i].stopped ? 'S' : 'R',
                    shell_jobs[i].cmd, shell_jobs[i].pid);
                found = 1;
            }
        }
    }
    if (!found) cmd_puts(out, "No jobs.\n");
}

/* ── nice ──────────────────────────────────────────────────────────────── */
static void cmd_nice(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 3) { kprintf("Usage: nice <thread_id> <priority>\n"); return; }
    uint64_t tid = 0;
    int      prio = 0;
    for (char* p = args[1]; *p; p++) tid  = tid  * 10 + (*p - '0');
    for (char* p = args[2]; *p; p++) prio = prio * 10 + (*p - '0');
    thread_t* t = sched_find_thread(tid);
    if (!t) { kprintf("nice: thread %lu not found\n", tid); return; }
    sched_set_priority(t, prio);
    kprintf("Thread %lu priority set to %d\n", tid, prio);
}

/* ── fault ─────────────────────────────────────────────────────────────── */
static void cmd_fault(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Fault injection:\n"
            "  1. Alloc until OOM\n"
            "  2. NULL deref (panic)\n"
            "Select [1-2]: ");
    char c = hal_uart_getchar();
    kprintf("%c\n", c);
    if (c == '1') {
        uint64_t pages[1024]; int np = 0;
        while (np < 1024) {
            pages[np] = pmm_alloc_page();
            if (!pages[np]) break;
            np++;
        }
        for (int i = 0; i < np; i++) pmm_free_page(pages[i]);
        kprintf("Allocated and freed %d pages\n", np);
    } else if (c == '2') {
        kprintf("Triggering NULL dereference...\n");
        volatile int* p = NULL; *p = 42;
    } else {
        kprintf("Invalid.\n");
    }
}

/* ── usermode / elfload / run ──────────────────────────────────────────── */
static void cmd_usermode(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    process_t* proc = process_create("usermode", 1);
    if (!proc) { kprintf("[usermode] failed to create process\n"); return; }
    uint8_t* d = (uint8_t*)_binary_build_user_program_elf_start;
    size_t   l = (uint64_t)_binary_build_user_program_elf_end
               - (uint64_t)_binary_build_user_program_elf_start;
    err_t e = process_exec(proc, d, l);
    if (e) { kprintf("[usermode] exec failed: %d\n", e); return; }
    kprintf("[usermode] pid=%d\n", proc->pid);
}

static void cmd_elfload(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    process_t* proc = process_create("elf-test", 1);
    if (!proc) { kprintf("Failed to create process\n"); return; }
    uint8_t* d = (uint8_t*)_binary_build_user_program_elf_start;
    size_t   l = (uint64_t)_binary_build_user_program_elf_end
               - (uint64_t)_binary_build_user_program_elf_start;
    kprintf("ELF at %p size %lu\n", d, l);
    err_t e = elf_load(proc, d, l);
    if (e) { kprintf("ELF load failed: %d\n", e); return; }
    kprintf("ELF loaded: entry=%llx\n", proc->entry_point);
}

static void cmd_run(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: run <filename>\n"); return; }
    char path[SHELL_LINE_BUF];
    resolve_path(args[1], path, SHELL_LINE_BUF);
    int fd = vfs_open(path, 0);
    if (fd < 0) { kprintf("run: '%s' not found\n", path); return; }
    uint64_t fsz = vfs_lseek(fd, 0, VFS_SEEK_END);
    vfs_lseek(fd, 0, VFS_SEEK_SET);
    if (!fsz || fsz > 1024 * 1024) {
        kprintf("run: bad file size\n"); vfs_close(fd); return;
    }
    uint8_t* buf = kmalloc(fsz);
    if (!buf) { kprintf("run: OOM\n"); vfs_close(fd); return; }
    vfs_read(fd, buf, fsz);
    vfs_close(fd);
    process_t* proc = process_create(args[1], 1);
    if (!proc) { kfree(buf); kprintf("run: create failed\n"); return; }
    err_t e = process_exec(proc, buf, fsz);
    if (e) kprintf("run: exec failed: %d\n", e);
    else   kprintf("[run] pid=%d '%s'\n", proc->pid, args[1]);
    kfree(buf);
}

/* ── pwd ───────────────────────────────────────────────────────────────── */
static void cmd_pwd(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out, "%s\n", cwd);
}

/* ── cd ────────────────────────────────────────────────────────────────── */
static void cmd_cd(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    const char* dest = (argc < 2) ? "/" : args[1];
    char resolved[SHELL_LINE_BUF];
    resolve_path(dest, resolved, SHELL_LINE_BUF);
    vfs_node_t* node = vfs_find(resolved);
    if (!node) { kprintf("cd: '%s' not found\n", resolved); return; }
    if (!(node->flags & 1)) { kprintf("cd: '%s' not a directory\n", resolved); return; }
    kstrncpy(cwd, resolved, SHELL_LINE_BUF - 1);
}

/* ── ls ────────────────────────────────────────────────────────────────── */
static void cmd_ls(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    const char* path  = cwd;
    int         long_ = 0;
    int         all_  = 0;
    for (int i = 1; i < argc; i++) {
        if (args[i][0] == '-') {
            for (char* f = args[i] + 1; *f; f++) {
                if (*f == 'l') long_ = 1;
                if (*f == 'a') all_  = 1;
            }
        } else {
            path = args[i];
        }
    }

    char resolved[SHELL_LINE_BUF];
    resolve_path(path, resolved, SHELL_LINE_BUF);
    vfs_node_t* dir = vfs_find(resolved);
    if (!dir) { cmd_printf(out, "ls: '%s' not found\n", resolved); return; }

    int found = 0;
    if (dir->fs && dir->fs->ops && dir->fs->ops->readdir) {
        if (all_) cmd_printf(out, "  .  (directory)\n  ..  (directory)\n");
        uint32_t idx = 0;
        while (1) {
            vfs_node_t* child = NULL;
            if (dir->fs->ops->readdir(dir, idx++, &child) != 0 || !child) break;
            if (!all_ && child->name[0] == '.') {
                kfree(child); continue;
            }
            int isdir = child->flags & 1;
            int issym = child->flags & VFS_FLAG_SYMLINK;
            if (long_) {
                char linkbuf[256];
                if (issym && child->fs && child->fs->ops && child->fs->ops->readlink &&
                    child->fs->ops->readlink(child, linkbuf, sizeof(linkbuf)) == 0)
                    cmd_printf(out, "  l  %-20s  %8llu bytes  -> %s\n",
                               child->name, child->size, linkbuf);
                else
                    cmd_printf(out, "  %c  %-20s  %8llu bytes\n",
                               isdir ? 'd' : '-', child->name, child->size);
            } else
                cmd_printf(out, "  %s%s%s\n", child->name, isdir ? "/" : "", issym ? "@" : "");
            kfree(child);
            found = 1;
        }
    }
    if (!found) {
        vfs_node_t* child = dir->children;
        while (child) {
            int isdir = child->flags & 1;
            int issym = child->flags & VFS_FLAG_SYMLINK;
            if (long_) {
                char linkbuf[256];
                if (issym && child->fs && child->fs->ops && child->fs->ops->readlink &&
                    child->fs->ops->readlink(child, linkbuf, sizeof(linkbuf)) == 0)
                    cmd_printf(out, "  l  %-20s  %8llu bytes  -> %s\n",
                               child->name, child->size, linkbuf);
                else
                    cmd_printf(out, "  %c  %-20s  %8llu bytes\n",
                               isdir ? 'd' : '-', child->name, child->size);
            } else
                cmd_printf(out, "  %s%s%s\n", child->name, isdir ? "/" : "", issym ? "@" : "");
            child = child->next;
        }
    }
}

/* ── cat ───────────────────────────────────────────────────────────────── */
static void cmd_cat(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    if (in && in->len > 0 && argc < 2) {
        if (out) pipe_buf_write(out, in->data, in->len);
        else     for (int i = 0; i < in->len; i++) kputchar(in->data[i]);
        return;
    }
    if (argc < 2) { cmd_puts(out, "Usage: cat <file>\n"); return; }
    char buf[512];
    for (int fi = 1; fi < argc; fi++) {
        char path[SHELL_LINE_BUF];
        resolve_path(args[fi], path, SHELL_LINE_BUF);
        int fd = vfs_open(path, 0);
        if (fd < 0) { cmd_printf(out, "cat: '%s' not found\n", path); continue; }
        int64_t n;
        while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
            if (out) pipe_buf_write(out, buf, (int)n);
            else { buf[n] = '\0'; kprintf("%s", buf); }
        }
        vfs_close(fd);
    }
    if (!out) kputchar('\n');
}

/* ── head / tail ───────────────────────────────────────────────────────── */
static void cmd_head(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    int n = 10;
    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (args[i][0] == '-' && args[i][1] >= '0' && args[i][1] <= '9') {
            n = 0; for (char* p = args[i] + 1; *p; p++) n = n * 10 + (*p - '0');
        } else if (kstrcmp(args[i], "-n") == 0 && i + 1 < argc) {
            n = 0; for (char* p = args[++i]; *p; p++) n = n * 10 + (*p - '0');
        } else { path = args[i]; }
    }

    char buf[4096]; int blen = 0;
    if (in && in->len > 0) {
        blen = in->len < 4095 ? in->len : 4095;
        kmemcpy(buf, in->data, blen);
    } else if (path) {
        char rp[SHELL_LINE_BUF]; resolve_path(path, rp, SHELL_LINE_BUF);
        int fd = vfs_open(rp, 0);
        if (fd < 0) { cmd_printf(out, "head: '%s' not found\n", rp); return; }
        int64_t r = vfs_read(fd, buf, sizeof(buf) - 1);
        blen = r > 0 ? (int)r : 0;
        vfs_close(fd);
    } else { cmd_puts(out, "Usage: head [-n N] <file>\n"); return; }

    int lines = 0; int i = 0;
    while (i < blen && lines < n) {
        char c = buf[i++];
        if (out) pipe_buf_write(out, &c, 1);
        else     kputchar(c);
        if (c == '\n') lines++;
    }
}

static void cmd_tail(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    int n = 10;
    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (args[i][0] == '-' && args[i][1] >= '0' && args[i][1] <= '9') {
            n = 0; for (char* p = args[i] + 1; *p; p++) n = n * 10 + (*p - '0');
        } else if (kstrcmp(args[i], "-n") == 0 && i + 1 < argc) {
            n = 0; for (char* p = args[++i]; *p; p++) n = n * 10 + (*p - '0');
        } else { path = args[i]; }
    }

    char buf[4096]; int blen = 0;
    if (in && in->len > 0) {
        blen = in->len < 4095 ? in->len : 4095;
        kmemcpy(buf, in->data, blen);
    } else if (path) {
        char rp[SHELL_LINE_BUF]; resolve_path(path, rp, SHELL_LINE_BUF);
        int fd = vfs_open(rp, 0);
        if (fd < 0) { cmd_printf(out, "tail: '%s' not found\n", rp); return; }
        int64_t r = vfs_read(fd, buf, sizeof(buf) - 1);
        blen = r > 0 ? (int)r : 0;
        vfs_close(fd);
    } else { cmd_puts(out, "Usage: tail [-n N] <file>\n"); return; }

    int lcount = 0;
    int start  = blen;
    while (start > 0 && lcount < n) {
        start--;
        if (buf[start] == '\n') lcount++;
    }
    if (start > 0 && buf[start] == '\n') start++;

    for (int i = start; i < blen; i++) {
        char c = buf[i];
        if (out) pipe_buf_write(out, &c, 1);
        else     kputchar(c);
    }
}

/* ── wc ────────────────────────────────────────────────────────────────── */
static void cmd_wc(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    char buf[4096]; int blen = 0;
    if (in && in->len > 0) {
        blen = in->len < 4095 ? in->len : 4095;
        kmemcpy(buf, in->data, blen);
    } else if (argc > 1) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
        int fd = vfs_open(rp, 0);
        if (fd < 0) { cmd_printf(out, "wc: '%s' not found\n", rp); return; }
        int64_t r = vfs_read(fd, buf, sizeof(buf) - 1);
        blen = r > 0 ? (int)r : 0;
        vfs_close(fd);
    } else { cmd_puts(out, "Usage: wc <file>  or  cmd | wc\n"); return; }

    int lines = 0, words = 0, chars = blen;
    int in_word = 0;
    for (int i = 0; i < blen; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1; words++;
        }
    }
    cmd_printf(out, "  %6d lines  %6d words  %6d chars\n", lines, words, chars);
}

/* ── grep (simple substring search) ───────────────────────────────────── */
static void cmd_grep(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    if (argc < 2) { cmd_puts(out, "Usage: grep <pattern> [file]\n"); return; }
    const char* pat = args[1];
    int         plen = kstrlen(pat);

    char buf[4096]; int blen = 0;
    if (argc < 3 && in && in->len > 0) {
        blen = in->len < 4095 ? in->len : 4095;
        kmemcpy(buf, in->data, blen);
    } else if (argc >= 3) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[2], rp, SHELL_LINE_BUF);
        int fd = vfs_open(rp, 0);
        if (fd < 0) { cmd_printf(out, "grep: '%s' not found\n", rp); return; }
        int64_t r = vfs_read(fd, buf, sizeof(buf) - 1);
        blen = r > 0 ? (int)r : 0;
        vfs_close(fd);
    } else { cmd_puts(out, "grep: no input\n"); return; }
    buf[blen] = '\0';

    char* line = buf;
    while (line < buf + blen) {
        char* nl = line;
        while (nl < buf + blen && *nl != '\n') nl++;
        *nl = '\0';
        int llen = nl - line;
        for (int i = 0; i <= llen - plen; i++) {
            if (kmemcmp(line + i, pat, (size_t)plen) == 0) {
                cmd_printf(out, "%s\n", line);
                break;
            }
        }
        line = nl + 1;
    }
}

/* ── find ──────────────────────────────────────────────────────────────── */
static void find_recurse(const char* dir_path, const char* name_pat,
                         pipe_buf_t* out, int depth) {
    if (depth > 16) return;
    vfs_node_t* dir = vfs_find(dir_path);
    if (!dir || !dir->fs || !dir->fs->ops || !dir->fs->ops->readdir) return;
    uint32_t idx = 0;
    while (1) {
        vfs_node_t* child = NULL;
        if (dir->fs->ops->readdir(dir, idx++, &child) != 0 || !child) break;
        char fpath[SHELL_LINE_BUF];
        kstrncpy(fpath, dir_path, SHELL_LINE_BUF - 2);
        if (fpath[kstrlen(fpath) - 1] != '/') kstrncat(fpath, "/", 1);
        kstrncat(fpath, child->name, SHELL_LINE_BUF - kstrlen(fpath) - 1);

        if (!name_pat || kstrstr(child->name, name_pat))
            cmd_printf(out, "%s\n", fpath);

        if (child->flags & 1)
            find_recurse(fpath, name_pat, out, depth + 1);

        kfree(child);
    }
}

static void cmd_find(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    const char* start = cwd;
    const char* name  = NULL;
    for (int i = 1; i < argc; i++) {
        if (kstrcmp(args[i], "-name") == 0 && i + 1 < argc) name = args[++i];
        else start = args[i];
    }
    char rp[SHELL_LINE_BUF]; resolve_path(start, rp, SHELL_LINE_BUF);
    find_recurse(rp, name, out, 0);
}

/* ── cp ────────────────────────────────────────────────────────────────── */
static void cmd_cp(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 3) { kprintf("Usage: cp <src> <dst>\n"); return; }
    char src[SHELL_LINE_BUF], dst[SHELL_LINE_BUF];
    resolve_path(args[1], src, SHELL_LINE_BUF);
    resolve_path(args[2], dst, SHELL_LINE_BUF);

    int fdsrc = vfs_open(src, O_RDONLY);
    if (fdsrc < 0) { kprintf("cp: '%s' not found\n", src); return; }

    int fddst = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (fddst < 0) { kprintf("cp: cannot open dst '%s'\n", dst); vfs_close(fdsrc); return; }

    char buf[512]; int64_t n;
    while ((n = vfs_read(fdsrc, buf, sizeof(buf))) > 0)
        vfs_write(fddst, buf, (uint64_t)n);
    vfs_close(fdsrc);
    vfs_close(fddst);
    kprintf("cp: '%s' -> '%s'\n", src, dst);
}

/* ── mv ────────────────────────────────────────────────────────────────── */
static void cmd_mv(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 3) { kprintf("Usage: mv <src> <dst>\n"); return; }
    char src[SHELL_LINE_BUF], dst[SHELL_LINE_BUF];
    resolve_path(args[1], src, SHELL_LINE_BUF);
    resolve_path(args[2], dst, SHELL_LINE_BUF);

    /* Try atomic rename first */
    if (vfs_rename(src, dst) == 0) {
        kprintf("mv: '%s' -> '%s'\n", src, dst);
        return;
    }

    /* Fallback: cp + unlink */
    char* mv_args[3] = { "cp", src, dst };
    cmd_cp(mv_args, 3, NULL, NULL);

    vfs_stat_t st;
    if (vfs_stat(dst, &st) == 0 && st.size > 0) {
        int r = vfs_unlink(src);
        if (r < 0) kprintf("mv: warning: could not remove src '%s'\n", src);
        else        kprintf("mv: '%s' -> '%s'\n", src, dst);
    }
}

/* ── ln ────────────────────────────────────────────────────────────────── */
static void cmd_ln(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 3) { kprintf("Usage: ln [-s] <target> <link>\n"); return; }
    int sym = 0;
    int src = 1;
    if (kstrcmp(args[1], "-s") == 0) { sym = 1; src = 2; }
    if (argc - src < 2) { kprintf("Usage: ln [-s] <target> <link>\n"); return; }
    char tgt[SHELL_LINE_BUF], link[SHELL_LINE_BUF];
    resolve_path(args[src], tgt, SHELL_LINE_BUF);
    resolve_path(args[src + 1], link, SHELL_LINE_BUF);
    if (sym) {
        if (vfs_symlink(args[src], link) == 0)
            kprintf("ln: '%s' -> '%s'\n", link, args[src]);
        else
            kprintf("ln: symlink failed\n");
    } else {
        if (vfs_link(tgt, link) == 0)
            kprintf("ln: '%s' -> '%s'\n", link, tgt);
        else
            kprintf("ln: failed\n");
    }
}

/* ── readlink ──────────────────────────────────────────────────────────── */
static void cmd_readlink(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { cmd_puts(out, "Usage: readlink <path>\n"); return; }
    char rp[SHELL_LINE_BUF];
    resolve_path(args[1], rp, SHELL_LINE_BUF);
    char buf[256];
    if (vfs_readlink(rp, buf, sizeof(buf)) == 0)
        cmd_printf(out, "%s\n", buf);
    else
        cmd_printf(out, "readlink: %s: not a symlink\n", args[1]);
}

/* ── touch ─────────────────────────────────────────────────────────────── */
static void cmd_touch(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: touch <path>\n"); return; }
    for (int i = 1; i < argc; i++) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[i], rp, SHELL_LINE_BUF);
        vfs_stat_t st;
        if (vfs_stat(rp, &st) == 0) continue;
        if (vfs_create(rp, 0) < 0) kprintf("touch: cannot create '%s'\n", rp);
    }
}

/* ── mkdir ─────────────────────────────────────────────────────────────── */
static void cmd_mkdir(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: mkdir <path>\n"); return; }
    for (int i = 1; i < argc; i++) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[i], rp, SHELL_LINE_BUF);
        if (vfs_mkdir(rp) < 0) kprintf("mkdir: failed '%s'\n", rp);
    }
}

/* ── chmod ────────────────────────────────────────────────────────────── */
static void cmd_chmod(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 3) { cmd_puts(out, "Usage: chmod <mode> <path>\n"); return; }

    uint32_t mode = 0;
    const char* p = args[1];
    while (*p >= '0' && *p <= '7') {
        mode = (mode << 3) | (uint32_t)(*p - '0');
        p++;
    }
    if (*p) { cmd_printf(out, "chmod: invalid mode '%s'\n", args[1]); return; }

    char rp[SHELL_LINE_BUF]; resolve_path(args[2], rp, SHELL_LINE_BUF);
    vfs_node_t* node = vfs_find(rp);
    if (!node) { cmd_printf(out, "chmod: '%s' not found\n", rp); return; }
    if (!node->fs || !node->fs->ops || !node->fs->ops->stat)
        { cmd_printf(out, "chmod: not supported on '%s'\n", rp); return; }

    vfs_stat_t st;
    if (vfs_stat(rp, &st) != 0)
        { cmd_printf(out, "chmod: cannot stat '%s'\n", rp); return; }
    if (vfs_chmod(rp, mode & 0xFFFF) == 0)
        cmd_printf(out, "chmod: %s -> %04x\n", rp, mode & 0xFFFF);
    else
        cmd_printf(out, "chmod: failed for '%s'\n", rp);
}

/* ── lock ─────────────────────────────────────────────────────────────── */
static void cmd_lock(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { cmd_puts(out, "Usage: lock <path>\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
    if (vfs_lock(rp) == 0)
        cmd_printf(out, "lock: '%s' locked\n", rp);
    else
        cmd_printf(out, "lock: '%s' not found\n", rp);
}

/* ── unlock ───────────────────────────────────────────────────────────── */
static void cmd_unlock(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { cmd_puts(out, "Usage: unlock <path>\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
    if (vfs_unlock(rp) == 0)
        cmd_printf(out, "unlock: '%s' unlocked\n", rp);
    else
        cmd_printf(out, "unlock: '%s' not found\n", rp);
}

/* ── edit (simple line-based editor) ──────────────────────────────────── */
#define EDIT_MAX_LINES 512
#define EDIT_MAX_LINE  256

static void cmd_edit(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { cmd_puts(out, "Usage: edit <path>\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
    vfs_stat_t st;
    if (vfs_stat(rp, &st) != 0) { cmd_printf(out, "edit: '%s' not found\n", rp); return; }

    char* lines[EDIT_MAX_LINES];
    int nlines = 0;
    kmemset(lines, 0, sizeof(lines));

    /* Read file into line array */
    int fd = vfs_open(rp, O_RDONLY);
    if (fd >= 0) {
        char buf[4096];
        int64_t nr = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (nr > 0) {
            buf[nr] = '\0';
            char* p = buf;
            while (*p && nlines < EDIT_MAX_LINES) {
                char* nl = p;
                while (*nl && *nl != '\n') nl++;
                int len = (int)(nl - p);
                char* line = kmalloc((size_t)(len + 1));
                if (!line) break;
                kmemcpy(line, p, (size_t)len);
                line[len] = '\0';
                lines[nlines++] = line;
                p = nl;
                if (*p == '\n') p++;
            }
        }
    }

    int dirty = 0;
    char input[SHELL_LINE_BUF];
    while (1) {
        cmd_puts(out, "> ");
        int n = 0;
        while (1) {
            int c = keyboard_getchar();
            if (c < 0) { thread_yield(); continue; }
            if (c == '\r' || c == '\n') { kputchar('\n'); break; }
            if (c == '\b' || c == 0x7F) {
                if (n > 0) { n--; kputchar('\b'); kputchar(' '); kputchar('\b'); }
            } else if (c >= ' ' && c <= '~') {
                if (n < SHELL_LINE_BUF - 1) { input[n++] = (char)c; kputchar((char)c); }
            }
        }
        if (n == 0) continue;
        input[n] = '\0';

        char* line = input;
        while (*line == ' ') line++;

        if (*line == 'q' && (line[1] == '\0' || line[1] == ' '))
            break;

        if (*line == 'w' && (line[1] == '\0' || line[1] == ' ')) {
            int wfd = vfs_open(rp, O_WRONLY | O_CREAT | O_TRUNC);
            if (wfd < 0) { cmd_printf(out, "edit: cannot write '%s'\n", rp); continue; }
            for (int i = 0; i < nlines; i++) {
                vfs_write(wfd, lines[i], kstrlen(lines[i]));
                vfs_write(wfd, "\n", 1);
            }
            vfs_close(wfd);
            dirty = 0;
            cmd_printf(out, "edit: written %d lines\n", nlines);
            continue;
        }

        if (*line == 'l' && (line[1] == '\0' || line[1] == ' ')) {
            for (int i = 0; i < nlines; i++)
                cmd_printf(out, "%4d: %s\n", i + 1, lines[i]);
            cmd_printf(out, "[%d lines]\n", nlines);
            continue;
        }

        if (*line == 'a' && (line[1] == ' ' || line[1] == '\t')) {
            char* text = line + 1;
            while (*text == ' ' || *text == '\t') text++;
            if (nlines >= EDIT_MAX_LINES) { cmd_puts(out, "edit: line limit\n"); continue; }
            char* newl = kmalloc(kstrlen(text) + 1);
            if (!newl) continue;
            kstrncpy(newl, text, kstrlen(text) + 1);
            lines[nlines++] = newl;
            dirty = 1;
            continue;
        }

        if (*line == 'e' && (line[1] == ' ' || line[1] == '\t')) {
            char* p = line + 1;
            int idx = 0;
            while (*p == ' ' || *p == '\t') p++;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
            if (idx < 1 || idx > nlines) { cmd_printf(out, "edit: line %d out of range\n", idx); continue; }
            while (*p == ' ' || *p == '\t') p++;
            kfree(lines[idx - 1]);
            char* newl = kmalloc(kstrlen(p) + 1);
            if (!newl) continue;
            kstrncpy(newl, p, kstrlen(p) + 1);
            lines[idx - 1] = newl;
            dirty = 1;
            continue;
        }

        if (*line == 'd' && (line[1] == ' ' || line[1] == '\t' || line[1] == '\0')) {
            int idx = -1;
            if (line[1] == ' ' || line[1] == '\t') {
                char* p = line + 1;
                while (*p == ' ' || *p == '\t') p++;
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                if (v >= 1 && v <= nlines) idx = v - 1;
            }
            if (idx < 0) { cmd_puts(out, "Usage: d <line_number>\n"); continue; }
            kfree(lines[idx]);
            for (int j = idx; j < nlines - 1; j++) lines[j] = lines[j + 1];
            nlines--;
            dirty = 1;
            continue;
        }

        cmd_printf(out, "edit: ? (l=list, e N text, d N, a text, w=write, q=quit)\n");
    }

    for (int i = 0; i < nlines; i++)
        if (lines[i]) kfree(lines[i]);

    if (dirty)
        cmd_puts(out, "edit: unsaved changes discarded\n");
}

/* ── rmdir ─────────────────────────────────────────────────────────────── */
static void cmd_rmdir(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: rmdir <path>\n"); return; }
    for (int i = 1; i < argc; i++) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[i], rp, SHELL_LINE_BUF);
        if (vfs_rmdir(rp) < 0) kprintf("rmdir: failed '%s'\n", rp);
    }
}

/* ── rm ────────────────────────────────────────────────────────────────── */
static void cmd_rm(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: rm <path> [path...]\n"); return; }
    for (int i = 1; i < argc; i++) {
        char rp[SHELL_LINE_BUF]; resolve_path(args[i], rp, SHELL_LINE_BUF);
        if (vfs_unlink(rp) < 0) kprintf("rm: failed '%s'\n", rp);
    }
}

/* ── stat ──────────────────────────────────────────────────────────────── */
static void cmd_statcmd(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { cmd_puts(out, "Usage: stat <path>\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
    vfs_stat_t st;
    if (vfs_stat(rp, &st) != 0) { cmd_printf(out, "stat: '%s' not found\n", rp); return; }
    uint32_t type = (st.mode >> 16) & 0xFFFF;
    cmd_printf(out,
        "  File : %s\n"
        "  Size : %llu bytes\n"
        "  Inode: %llu\n"
        "  Mode : %04x\n"
        "  Type : %s\n"
        "  Flags: %x\n"
        "  atime: %llu\n"
        "  mtime: %llu\n"
        "  ctime: %llu\n",
        rp, st.size, st.inode, st.mode,
        (type == 2) ? "directory" :
        (type == 3) ? "symlink" : "regular",
        st.flags,
        st.atime, st.mtime, st.ctime);
    if (type == 3) {
        char lbuf[256];
        if (vfs_readlink(rp, lbuf, sizeof(lbuf)) == 0)
            cmd_printf(out, "  -> %s\n", lbuf);
    }
}

/* ── writefile ─────────────────────────────────────────────────────────── */
static void cmd_writefile(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)out;
    if (argc < 2) { kprintf("Usage: writefile <path> [text...]\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);

    int fd = vfs_open(rp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { kprintf("writefile: cannot open '%s'\n", rp); return; }

    if (in && in->len > 0) {
        vfs_write(fd, in->data, (uint64_t)in->len);
    } else {
        for (int i = 2; i < argc; i++) {
            vfs_write(fd, args[i], kstrlen(args[i]));
            if (i < argc - 1) vfs_write(fd, " ", 1);
        }
        vfs_write(fd, "\n", 1);
    }
    vfs_close(fd);
    kprintf("writefile: ok\n");
}

/* ── kbtest ────────────────────────────────────────────────────────────── */
static void cmd_kbtest(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    kprintf("Keyboard test (ESC to exit):\n");
    while (1) {
        int c = keyboard_getchar();
        if (c == 27) break;
        kputchar(c);
    }
    kprintf("\nDone.\n");
}

/* ── atatest ────────────────────────────────────────────────────────────── */
static void cmd_atatest(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    for (int i = 0; i < 4; i++) {
        if (!ata_drive_present(i)) continue;
        cmd_printf(out, "ATA %d: %s\n"
                        "  Sectors: %llu (%llu MB)\n",
                   i, ata_drive_model(i),
                   ata_drive_sectors(i), ata_drive_sectors(i) / 2048);
    }
}

/* ── format / mount ────────────────────────────────────────────────────── */
static void cmd_format(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in; (void)out;
    block_dev_t* bdev = block_find("ramdisk");
    if (!bdev) { kprintf("format: no ramdisk\n"); return; }
    err_t e = sfs_format(bdev);
    if (e) { kprintf("format: failed (%d)\n", e); return; }
    e = sfs_mount(bdev);
    if (e) { kprintf("format: remount failed (%d)\n", e); return; }
    kprintf("SFS formatted and remounted.\n");
}

static void cmd_mount(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    cmd_printf(out, "Block devices:\n");
    for (int i = 0; i < block_count(); i++) {
        block_dev_t* d = block_get(i);
        cmd_printf(out, "  %s (%llu blocks)\n", d->name, d->block_count);
    }
}

/* ── history ───────────────────────────────────────────────────────────── */
static void cmd_history(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc > 1 && kstrcmp(args[1], "-c") == 0) {
        shell_history_clear(); return;
    }
    int start = (hist_count > SHELL_HISTORY) ? hist_count - SHELL_HISTORY : 0;
    for (int i = start; i < hist_count; i++)
        cmd_printf(out, "  %3d  %s\n", i + 1, history[i % SHELL_HISTORY]);
}

/* ── alias / unalias ───────────────────────────────────────────────────── */
static void cmd_alias(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) {
        for (int i = 0; i < alias_count; i++)
            cmd_printf(out, "  alias %s='%s'\n", aliases[i].name, aliases[i].value);
        return;
    }
    /* Join args[1..] with spaces to handle `alias ll="ls -la"` tokenization */
    char joined[SHELL_LINE_BUF];
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        const char* s = args[i];
        while (*s && pos < SHELL_LINE_BUF - 1) {
            if (*s == '"') { s++; continue; }
            joined[pos++] = *s++;
        }
        if (i < argc - 1 && pos < SHELL_LINE_BUF - 1) joined[pos++] = ' ';
    }
    joined[pos] = '\0';

    char* eq = joined;
    while (*eq && *eq != '=') eq++;
    if (!*eq) { cmd_printf(out, "Usage: alias name=value\n"); return; }
    *eq = '\0';
    shell_alias_set(joined, eq + 1);
}

static void cmd_unalias(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: unalias <name>\n"); return; }
    shell_alias_del(args[1]);
}

/* ── set / unset / printenv ────────────────────────────────────────────── */
static void cmd_set(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in;
    if (argc < 2) { shell_printenv(); return; }
    char* eq = args[1];
    while (*eq && *eq != '=') eq++;
    if (!*eq) { cmd_printf(out, "Usage: set NAME=VALUE\n"); return; }
    *eq = '\0';
    shell_setenv(args[1], eq + 1);
}

static void cmd_unset(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: unset <NAME>\n"); return; }
    shell_unsetenv(args[1]);
}

static void cmd_printenv(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)args; (void)argc; (void)in;
    for (int i = 0; i < env_count; i++)
        cmd_printf(out, "%s=%s\n", env_vars[i].name, env_vars[i].val);
}

/* ── source ────────────────────────────────────────────────────────────── */
static void cmd_source(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    if (argc < 2) { kprintf("Usage: source <script>\n"); return; }
    char rp[SHELL_LINE_BUF]; resolve_path(args[1], rp, SHELL_LINE_BUF);
    if (shell_source(rp) < 0) kprintf("source: error in '%s'\n", rp);
}

/* ── type ─────────────────────────────────────────────────────────────── */
static void cmd_type(char** args, int argc, pipe_buf_t* in, pipe_buf_t* out) {
    (void)in; (void)out;
    for (int i = 1; i < argc; i++) {
        const char* name = args[i];
        int found = 0;

        const char* alias_val = shell_alias_get(name);
        if (alias_val) {
            cmd_printf(out, "%s is aliased to '%s'\n", name, alias_val);
            found = 1;
        }

        for (int j = 0; commands[j].name && !found; j++) {
            if (kstrcmp(commands[j].name, name) == 0) {
                cmd_printf(out, "%s is a shell built-in (%s)\n", name, commands[j].desc);
                found = 1;
            }
        }

        if (!found) {
            char rp[SHELL_LINE_BUF];
            resolve_path(name, rp, SHELL_LINE_BUF);
            vfs_stat_t st;
            if (vfs_stat(rp, &st) == 0) {
                cmd_printf(out, "%s is %s (%llu bytes)\n", name, rp, st.size);
                found = 1;
            }
        }

        if (!found)
            cmd_printf(out, "%s not found\n", name);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Command table
 * ══════════════════════════════════════════════════════════════════════════ */

static shell_cmd_t commands[] = {
    {"help",      cmd_help,      "Show command overview"},
    {"echo",      cmd_echo,      "Print text (-n: no newline)"},
    {"clear",     cmd_clear,     "Clear screen"},
    {"history",   cmd_history,   "Show/clear history (-c)"},
    {"alias",     cmd_alias,     "Define alias  alias name=value"},
    {"unalias",   cmd_unalias,   "Remove alias"},
    {"set",       cmd_set,       "Set env variable  set NAME=VALUE"},
    {"unset",     cmd_unset,     "Unset env variable"},
    {"printenv",  cmd_printenv,  "Print environment variables"},
    {"source",    cmd_source,    "Execute a shell script"},
    {"type",      cmd_type,      "Display command type (alias/builtin/file)"},
    /* Filesystem */
    {"pwd",       cmd_pwd,       "Print working directory"},
    {"cd",        cmd_cd,        "Change directory"},
    {"ls",        cmd_ls,        "List directory (-l long, -a all)"},
    {"cat",       cmd_cat,       "Print file(s)"},
    {"head",      cmd_head,      "First N lines (-n N)"},
    {"tail",      cmd_tail,      "Last N lines (-n N)"},
    {"wc",        cmd_wc,        "Count lines/words/chars"},
    {"grep",      cmd_grep,      "Search lines matching pattern"},
    {"find",      cmd_find,      "Find files  find [dir] [-name pat]"},
    {"cp",        cmd_cp,        "Copy file"},
    {"mv",        cmd_mv,        "Move/rename file"},
    {"ln",        cmd_ln,        "Create hard link or symlink (-s)"},
    {"readlink",  cmd_readlink,  "Read symlink target"},
    {"touch",     cmd_touch,     "Create or update file"},
    {"mkdir",     cmd_mkdir,     "Create directory"},
    {"rmdir",     cmd_rmdir,     "Remove empty directory"},
    {"chmod",     cmd_chmod,     "Change file mode (octal, e.g. 0644)"},
    {"lock",      cmd_lock,      "Lock file (advisory write lock)"},
    {"unlock",    cmd_unlock,    "Unlock file (advisory write lock)"},
    {"edit",      cmd_edit,      "Simple line-based file editor"},
    {"rm",        cmd_rm,        "Remove file(s)"},
    {"stat",      cmd_statcmd,   "Show file metadata"},
    {"writefile", cmd_writefile, "Write text to file"},
    /* System info */
    {"meminfo",   cmd_meminfo,   "Show memory usage"},
    {"uptime",    cmd_uptime,    "Show system uptime"},
    {"stats",     cmd_stats,     "Show kernel statistics"},
    {"version",   cmd_version,   "Show OS version"},
    {"uname",     cmd_uname,     "Show system name/version"},
    /* Process management */
    {"ps",        cmd_ps,        "List threads"},
    {"top",       cmd_top,       "System overview"},
    {"demo",      cmd_demo,      "Run 3 demo threads"},
    {"compute",   cmd_compute,   "Spawn compute threads [n]"},
    {"mutex",     cmd_mutex,     "Mutex contention test"},
    {"event",     cmd_event,     "Event bus stress test"},
    {"cleanup",   cmd_cleanup,   "Reap zombie threads"},
    {"kill",      cmd_kill,      "Send signal to process"},
    {"bg",        cmd_bg,        "Continue job in background"},
    {"fg",        cmd_fg,        "Bring job to foreground"},
    {"jobs",      cmd_jobs,      "List background jobs"},
    {"nice",      cmd_nice,      "Set thread priority"},
    {"run",       cmd_run,       "Run ELF from VFS"},
    {"usermode",  cmd_usermode,  "Launch embedded user program"},
    {"elfload",   cmd_elfload,   "Test ELF loader"},
    /* Hardware / storage */
    {"kbtest",    cmd_kbtest,    "Keyboard input test"},
    {"atatest",   cmd_atatest,   "Show ATA drives"},
    {"format",    cmd_format,    "Format+remount SFS on ramdisk"},
    {"mount",     cmd_mount,     "List block devices"},
    /* Danger zone */
    {"reboot",    cmd_reboot,    "Reboot system"},
    {"poweroff",  cmd_poweroff,  "Power off system"},
    {"panic",     cmd_panic,     "Trigger kernel panic (test)"},
    {"fault",     cmd_fault,     "Fault injection test"},
    {NULL, NULL, NULL}
};

/* ══════════════════════════════════════════════════════════════════════════
 * Pipeline executor
 * ══════════════════════════════════════════════════════════════════════════ */

static void exec_stage(pipeline_stage_t* st,
                       pipe_buf_t* pipe_in,
                       pipe_buf_t* pipe_out) {
    if (st->argc == 0) return;

    const char* cmd_name = st->argv[0];
    const char* aliased  = shell_alias_get(cmd_name);
    char alias_buf[SHELL_LINE_BUF];
    char* alias_argv[SHELL_MAX_ARGS];
    int   alias_argc = 0;

    if (aliased) {
        kstrncpy(alias_buf, aliased, SHELL_LINE_BUF - 1);
        char* p = alias_buf;
        while (*p && alias_argc < SHELL_MAX_ARGS - st->argc - 1) {
            while (*p == ' ') p++;
            if (!*p) break;
            alias_argv[alias_argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) { *p = '\0'; p++; }
        }
        for (int i = 1; i < st->argc && alias_argc < SHELL_MAX_ARGS - 1; i++)
            alias_argv[alias_argc++] = st->argv[i];
        alias_argv[alias_argc] = NULL;
        cmd_name = alias_argv[0];
        for (int i = 0; i < alias_argc && i < SHELL_MAX_ARGS; i++)
            st->argv[i] = alias_argv[i];
        st->argv[alias_argc] = NULL;
        st->argc  = alias_argc;
    }

    pipe_buf_t  redir_in_buf;
    pipe_buf_t  redir_out_buf;
    kmemset(&redir_in_buf, 0, sizeof(redir_in_buf));
    kmemset(&redir_out_buf, 0, sizeof(redir_out_buf));
    pipe_buf_t* eff_in  = pipe_in;
    pipe_buf_t* eff_out = pipe_out;

    if (st->redir_in) {
        char rp[SHELL_LINE_BUF]; resolve_path(st->redir_in, rp, SHELL_LINE_BUF);
        int fd = vfs_open(rp, O_RDONLY);
        if (fd < 0) { kprintf("shell: cannot open '%s' for reading\n", rp); return; }
        int64_t n = vfs_read(fd, redir_in_buf.data, SHELL_PIPE_BUF - 1);
        redir_in_buf.len = n > 0 ? (int)n : 0;
        vfs_close(fd);
        eff_in = &redir_in_buf;
    }

    int redir_out_fd = -1;
    if (st->redir_out) {
        char rp[SHELL_LINE_BUF]; resolve_path(st->redir_out, rp, SHELL_LINE_BUF);
        int out_flags = O_WRONLY | O_CREAT;
        if (st->redir_append)
            out_flags |= O_APPEND;
        else
            out_flags |= O_TRUNC;
        redir_out_fd = vfs_open(rp, out_flags);
        if (redir_out_fd < 0) { kprintf("shell: cannot open '%s' for writing\n", rp); return; }
        eff_out = &redir_out_buf;
    }

    int found = 0;
    for (int i = 0; commands[i].name; i++) {
        if (kstrcmp(commands[i].name, cmd_name) == 0) {
            commands[i].func(st->argv, st->argc, eff_in, eff_out);
            found = 1;
            break;
        }
    }
    if (!found) kprintf("Unknown command: %s\n", cmd_name);

    if (redir_out_fd >= 0) {
        vfs_write(redir_out_fd, redir_out_buf.data, (uint64_t)redir_out_buf.len);
        vfs_close(redir_out_fd);
    }
}

static void exec_pipeline(pipeline_stage_t* stages, int nstages) {
    if (nstages == 1) {
        exec_stage(&stages[0], NULL, NULL);
        return;
    }

    pipe_buf_t* bufs = kmalloc(sizeof(pipe_buf_t) * (nstages - 1));
    if (!bufs) { kprintf("shell: OOM for pipe buffers\n"); return; }
    kmemset(bufs, 0, sizeof(pipe_buf_t) * (nstages - 1));

    for (int i = 0; i < nstages; i++) {
        pipe_buf_t* in  = (i == 0)           ? NULL       : &bufs[i - 1];
        pipe_buf_t* out = (i == nstages - 1) ? NULL       : &bufs[i];
        exec_stage(&stages[i], in, out);
    }
    kfree(bufs);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Line processing
 * ══════════════════════════════════════════════════════════════════════════ */

static void process_line(const char* line) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return;

    char expanded[SHELL_LINE_BUF];
    expand_vars(line, expanded, SHELL_LINE_BUF);

    char work[SHELL_LINE_BUF];
    kstrncpy(work, expanded, SHELL_LINE_BUF - 1);

    pipeline_stage_t stages[SHELL_MAX_PIPE];
    int nstages = parse_pipeline(work, stages);
    if (nstages == 0) return;

    exec_pipeline(stages, nstages);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Script / source
 * ══════════════════════════════════════════════════════════════════════════ */

int shell_source(const char* path) {
    if (script_depth >= SHELL_SCRIPT_DEPTH) {
        kprintf("source: max nesting depth reached\n"); return -1;
    }
    char rp[SHELL_LINE_BUF]; resolve_path(path, rp, SHELL_LINE_BUF);
    int fd = vfs_open(rp, 0);
    if (fd < 0) { kprintf("source: '%s' not found\n", rp); return -1; }

    script_depth++;
    char buf[SHELL_LINE_BUF]; int bpos = 0;
    int64_t n;
    char rd[1];
    while ((n = vfs_read(fd, rd, 1)) == 1) {
        if (rd[0] == '\n' || bpos == SHELL_LINE_BUF - 1) {
            buf[bpos] = '\0';
            if (bpos > 0) process_line(buf);
            bpos = 0;
        } else {
            buf[bpos++] = rd[0];
        }
    }
    if (bpos > 0) { buf[bpos] = '\0'; process_line(buf); }
    vfs_close(fd);
    script_depth--;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Line editor
 * ══════════════════════════════════════════════════════════════════════════ */

static void redraw_suffix(void) {
    for (int i = line_pos; i < line_len; i++) kputchar(line_buf[i]);
    for (int i = line_pos; i < line_len; i++) kprintf("\b");
}

static void editor_insert(char c) {
    if (line_len >= SHELL_LINE_BUF - 1) return;
    for (int i = line_len; i > line_pos; i--) line_buf[i] = line_buf[i - 1];
    line_buf[line_pos++] = c;
    line_len++;
    kputchar(c);
    redraw_suffix();
}

static void editor_backspace(void) {
    if (line_pos == 0) return;
    for (int i = line_pos - 1; i < line_len - 1; i++) line_buf[i] = line_buf[i + 1];
    line_pos--; line_len--;
    line_buf[line_len] = '\0';
    kprintf("\b");
    redraw_suffix();
    kputchar(' ');
    kprintf("\b");
    for (int i = line_pos; i < line_len; i++) kprintf("\b");
}

static void editor_delete_at(void) {
    if (line_pos >= line_len) return;
    for (int i = line_pos; i < line_len - 1; i++) line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = '\0';
    redraw_suffix();
    kputchar(' ');
    for (int i = line_pos; i <= line_len; i++) kprintf("\b");
}

static void editor_move_left(void) {
    if (line_pos > 0) { line_pos--; kprintf("\b"); }
}

static void editor_move_right(void) {
    if (line_pos < line_len) { kputchar(line_buf[line_pos]); line_pos++; }
}

static void editor_home(void) {
    while (line_pos > 0) { kprintf("\b"); line_pos--; }
}

static void editor_end(void) {
    while (line_pos < line_len) { kputchar(line_buf[line_pos]); line_pos++; }
}

static void editor_clear_line(void) {
    editor_home();
    for (int i = 0; i < line_len; i++) kputchar(' ');
    editor_home();
    line_len = 0;
}

static void editor_load(const char* s) {
    editor_clear_line();
    for (const char* p = s; *p && line_len < SHELL_LINE_BUF - 1; p++) {
        line_buf[line_len++] = *p;
        kputchar(*p);
    }
    line_pos = line_len;
}

static void editor_kill_to_end(void) {
    int kill_len = line_len - line_pos;
    for (int i = 0; i < kill_len; i++) kputchar(' ');
    for (int i = 0; i < kill_len; i++) kprintf("\b");
    line_len = line_pos;
}

static void editor_delete_word_left(void) {
    if (line_pos == 0) return;
    int p = line_pos;
    while (p > 0 && line_buf[p - 1] == ' ') p--;
    while (p > 0 && line_buf[p - 1] != ' ') p--;
    int deleted = line_pos - p;
    for (int i = 0; i < deleted; i++) kprintf("\b");
    for (int i = p; i < line_len - deleted; i++) line_buf[i] = line_buf[i + deleted];
    line_len -= deleted; line_pos = p;
    redraw_suffix();
    for (int i = 0; i < deleted; i++) kputchar(' ');
    for (int i = 0; i <= deleted + (line_len - line_pos); i++) kprintf("\b");
    for (int i = line_pos; i < line_len; i++) kprintf("\b");
}

/* ══════════════════════════════════════════════════════════════════════════
 * shell_init / shell_run
 * ══════════════════════════════════════════════════════════════════════════ */

void shell_init(void) {
    shell_setenv("OS",   "OPERtur");
    shell_setenv("VER",  "0.1.0");
    shell_setenv("HOME", "/");
    kstrncpy(cwd, "/", SHELL_LINE_BUF - 1);
    kprintf("[SHELL] Initialised\n");
}

void shell_run(void) {
    kprintf("\n");
    kprintf("╔══════════════════════════════════════════════╗\n");
    kprintf("║  OPERtur/TRY1 OS v0.1.0                      ║\n");
    kprintf("║  Layered x86-64 Kernel                        ║\n");
    kprintf("║  Type 'help' for commands                     ║\n");
    kprintf("║  Pipes: cmd|cmd   Redir: >f >>f <f            ║\n");
    kprintf("║  Vars:  $NAME     Aliases: alias n=v          ║\n");
    kprintf("╚══════════════════════════════════════════════╝\n");

    line_pos = line_len = 0;
    hist_nav = hist_count;

    kprintf(SHELL_PROMPT);

    for (;;) {
        eventbus_dispatch();
        if (need_reschedule) schedule();

        int c = hal_uart_getchar();
        if (c < 0) { thread_yield(); continue; }

        if (c == 0x1B) {
            int b = hal_uart_getchar();
            if (b == '[') {
                int d = hal_uart_getchar();
                if (d == 'A') {
                    if (hist_count == 0) continue;
                    if (hist_nav == hist_count) {
                        line_buf[line_len] = '\0';
                        kstrncpy(hist_saved, line_buf, SHELL_LINE_BUF - 1);
                    }
                    if (hist_nav > 0 &&
                        hist_nav > (hist_count > SHELL_HISTORY
                                    ? hist_count - SHELL_HISTORY : 0))
                        hist_nav--;
                    editor_load(history[hist_nav % SHELL_HISTORY]);

                } else if (d == 'B') {
                    if (hist_nav >= hist_count) continue;
                    hist_nav++;
                    if (hist_nav == hist_count)
                        editor_load(hist_saved);
                    else
                        editor_load(history[hist_nav % SHELL_HISTORY]);

                } else if (d == 'C') {
                    editor_move_right();
                } else if (d == 'D') {
                    editor_move_left();
                } else if (d == 'H') {
                    editor_home();
                } else if (d == 'F') {
                    editor_end();
                } else if (d == '3') {
                    int e2 = hal_uart_getchar();
                    if (e2 == '~') editor_delete_at();
                } else if (d == '1') {
                    int e2 = hal_uart_getchar();
                    if (e2 == '~') editor_home();
                } else if (d == '4') {
                    int e2 = hal_uart_getchar();
                    if (e2 == '~') editor_end();
                }
            } else if (b == 'O') {
                int d = hal_uart_getchar();
                if (d == 'H') editor_home();
                else if (d == 'F') editor_end();
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            line_buf[line_len] = '\0';
            if (line_len > 0) {
                add_history(line_buf);
                process_line(line_buf);
            }
            line_pos = line_len = 0;
            hist_nav = hist_count;
            thread_yield();
            kprintf(SHELL_PROMPT);

        } else if (c == '\b' || c == 127) {
            editor_backspace();

        } else if (c == '\t') {
            line_buf[line_len] = '\0';
            do_tab_complete();

        } else if (c == 1) {
            editor_home();
        } else if (c == 5) {
            editor_end();
        } else if (c == 3) {
            kprintf("^C\n");
            line_pos = line_len = 0;
            kprintf(SHELL_PROMPT);
        } else if (c == 21) {
            editor_clear_line();
        } else if (c == 11) {
            editor_kill_to_end();
        } else if (c == 23) {
            editor_delete_word_left();
        } else if (c >= ' ' && c <= '~') {
            editor_insert((char)c);
        }
    }
}
