#ifndef KERNEL_ERRNO_H
#define KERNEL_ERRNO_H

#include "types.h"

static inline const char* err_str(err_t e) {
    switch (e) {
        case ERR_OK:       return "Success";
        case ERR_GENERAL:  return "General error";
        case ERR_NOMEM:    return "Out of memory";
        case ERR_INVAL:    return "Invalid argument";
        case ERR_BADADDR:  return "Bad address";
        case ERR_BUSY:     return "Resource busy";
        case ERR_TIMEOUT:  return "Operation timed out";
        case ERR_AGAIN:    return "Try again";
        case ERR_FAULT:    return "Page fault";
        case ERR_NOSYS:    return "Not implemented";
        case ERR_PERM:     return "Permission denied";
        case ERR_EXIST:    return "File exists";
        case ERR_NOENT:    return "No such entry";
        case ERR_IO:       return "I/O error";
        case ERR_NOSPACE:  return "No space left";
        case ERR_NAMETOOLONG: return "Name too long";
        case ERR_LOOP:     return "Symlink loop";
        case ERR_STALE:    return "Stale handle";
        case ERR_DEADLOCK: return "Deadlock detected";
        case ERR_CAP:      return "Capability denied";
        case ERR_BADFD:    return "Bad file descriptor";
        default:           return "Unknown error";
    }
}

#endif
