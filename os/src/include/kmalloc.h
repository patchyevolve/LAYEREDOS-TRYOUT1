#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

void kmalloc_init(void);
void* kmalloc(size_t size);
void  kfree(void* ptr);
size_t kmalloc_used(void);
size_t kmalloc_total(void);

#endif
