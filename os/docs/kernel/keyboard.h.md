# keyboard.h — PS/2 Keyboard Driver Interface

**Path:** `os/src/kernel/keyboard.h`  
**Layer:** Layer 1 (HAL) — header

---

## Purpose

Declares the keyboard driver's public API.  Included by `shell.c` for the
`kbtest` command and by any future code that needs keyboard input.

---

## Constant

```c
#define KEYBUF_SIZE 256   // ring buffer capacity in characters
```

---

## API

```c
void keyboard_init(void);
int  keyboard_getchar(void);         // blocking; returns next ASCII char
int  keyboard_data_available(void);  // non-blocking; 1 if char ready
void keyboard_irq_handler(int_frame_t* frame, void* data);  // IRQ1 handler
```

### Usage pattern

```c
// At init time:
keyboard_init();   // registers IRQ handler automatically

// To read (blocking):
int c = keyboard_getchar();

// To poll (non-blocking):
if (keyboard_data_available())
    int c = keyboard_getchar();
```

The IRQ handler signature matches `irq_handler_t` from `hal.h` so it can
be passed directly to `hal_irq_register`.  `keyboard_init` calls
`hal_irq_register(1, keyboard_irq_handler, NULL)` internally.
