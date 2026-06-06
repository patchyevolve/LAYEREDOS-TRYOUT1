# errno.h — Error Code String Table

**Path:** `os/src/include/errno.h`  
**Layer:** Cross-cutting

---

## Purpose

Provides `err_str(err_t e)` — a single function that converts any `err_t`
error code to a human-readable string.  Used in debug prints, shell output,
and `kpanic` messages to avoid sprinkling string literals throughout the
codebase.

---

## Design decisions

**`static inline`** — the function is defined in a header (not a `.c` file).
Every translation unit that includes `errno.h` gets its own copy, which the
compiler will typically inline and optimise to a direct string load.
This avoids the need for a separate `errno.o` object and a `kstrdup` call.

**Switch, not table** — a switch on `err_t` values lets the compiler verify
at compile time that all enum cases are handled (with `-Wswitch`).  A table
of `const char*` indexed by `-(int)e` would be fragile if error codes
were ever renumbered.

**`default` case returns `"Unknown error"`** — future error codes added to
`err_t` in `types.h` will not silently return garbage.

---

## Usage

```c
err_t e = pmm_init(...);
if (e != ERR_OK)
    kprintf("PMM init failed: %s\n", err_str(e));
```

---

## All mappings

| err_t constant | String |
|----------------|--------|
| `ERR_OK` | "Success" |
| `ERR_GENERAL` | "General error" |
| `ERR_NOMEM` | "Out of memory" |
| `ERR_INVAL` | "Invalid argument" |
| `ERR_BADADDR` | "Bad address" |
| `ERR_BUSY` | "Resource busy" |
| `ERR_TIMEOUT` | "Operation timed out" |
| `ERR_AGAIN` | "Try again" |
| `ERR_FAULT` | "Page fault" |
| `ERR_NOSYS` | "Not implemented" |
| `ERR_PERM` | "Permission denied" |
| `ERR_EXIST` | "File exists" |
| `ERR_NOENT` | "No such entry" |
| `ERR_IO` | "I/O error" |
| `ERR_NOSPACE` | "No space left" |
| `ERR_NAMETOOLONG` | "Name too long" |
| `ERR_LOOP` | "Symlink loop" |
| `ERR_STALE` | "Stale handle" |
| `ERR_DEADLOCK` | "Deadlock detected" |
| `ERR_CAP` | "Capability denied" |
| `ERR_BADFD` | "Bad file descriptor" |
| (anything else) | "Unknown error" |
