#ifndef SHELL_H
#define SHELL_H

#include "types.h"

/* ── Buffer / history sizes ─────────────────────────────────────────────── */
#define SHELL_HISTORY      64          /* number of history entries kept    */
#define SHELL_LINE_BUF     512         /* max chars per input line          */
#define SHELL_MAX_ARGS     32          /* max tokens per command            */
#define SHELL_MAX_PIPE     8           /* max stages in a pipeline          */
#define SHELL_PIPE_BUF     4096        /* bytes in each inter-stage buffer  */
#define SHELL_ALIAS_MAX    32          /* max number of aliases             */
#define SHELL_ENV_MAX      64          /* max shell environment variables   */
#define SHELL_ENV_NAME     32          /* max length of a variable name     */
#define SHELL_ENV_VAL      256         /* max length of a variable value    */
#define SHELL_SCRIPT_DEPTH 8           /* max nested script/source depth    */

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
void shell_init(void);
void shell_run(void);

/* ── Pipe / redirection engine (used by built-ins that want to be pipeable) */

/*
 * A pipe_buf_t is the in-memory byte-stream passed between pipe stages.
 * Built-in commands receive a pointer to their stdin buffer and write their
 * stdout into the supplied output buffer.  Either pointer may be NULL (stdin
 * = nothing,  stdout = discard / print to console).
 */
typedef struct pipe_buf {
    char   data[SHELL_PIPE_BUF];
    int    len;     /* bytes currently in data[]   */
    int    pos;     /* read cursor                 */
} pipe_buf_t;

/* Append bytes to a pipe buffer (silently truncates at SHELL_PIPE_BUF). */
void pipe_buf_write(pipe_buf_t* buf, const char* data, int len);
/* Read one byte; returns -1 on EOF. */
int  pipe_buf_readc(pipe_buf_t* buf);

/* ── Alias API (usable by other kernel subsystems if needed) ────────────── */
typedef struct shell_alias {
    char name[SHELL_ENV_NAME];
    char value[SHELL_ENV_VAL];
} shell_alias_t;

void        shell_alias_set(const char* name, const char* value);
const char* shell_alias_get(const char* name);
void        shell_alias_del(const char* name);
void        shell_alias_list(void);

/* ── Environment variable API ───────────────────────────────────────────── */
void        shell_setenv(const char* name, const char* value);
const char* shell_getenv(const char* name);
void        shell_unsetenv(const char* name);
void        shell_printenv(void);

/* ── History API ────────────────────────────────────────────────────────── */
void shell_history_print(void);
void shell_history_clear(void);

/* ── Script execution ───────────────────────────────────────────────────── */
/* Execute a shell script file from the VFS. Returns 0 on success. */
int shell_source(const char* path);

#endif /* SHELL_H */
