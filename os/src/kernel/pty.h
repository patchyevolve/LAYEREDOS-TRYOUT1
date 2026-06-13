#ifndef PTY_H
#define PTY_H

#include "types.h"

#define PTY_MAX 8
#define PTY_BUF_SIZE 4096
#define PTY_LINE_BUF_SIZE 256
#define PTY_CC_NCCS 32

int pty_pair_create(int fds[2]);
void pty_init(void);

#endif
