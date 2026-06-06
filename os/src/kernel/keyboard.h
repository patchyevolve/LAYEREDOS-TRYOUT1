#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"
#include "hal.h"

#define KEYBUF_SIZE 256

void keyboard_init(void);
int keyboard_getchar(void);
int keyboard_data_available(void);
void keyboard_irq_handler(int_frame_t* frame, void* data);

#endif
