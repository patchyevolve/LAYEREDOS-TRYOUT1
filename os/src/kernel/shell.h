#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#define SHELL_HISTORY 16
#define SHELL_LINE_BUF 256

void shell_init(void);
void shell_run(void);

#endif
