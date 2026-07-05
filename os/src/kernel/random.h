#ifndef RANDOM_H
#define RANDOM_H

#include "types.h"

void random_init(void);
void random_get_bytes(uint8_t* buf, size_t count);

#endif
