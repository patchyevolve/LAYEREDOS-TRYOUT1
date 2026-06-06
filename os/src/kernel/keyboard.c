#include "kernel.h"
#include "keyboard.h"
#include "hal.h"

#define PS2_DATA   0x60
#define PS2_CMD    0x64
#define PS2_STATUS 0x64

#define KB_IRQ 1

static volatile char keybuf[KEYBUF_SIZE];
static volatile int  keybuf_head = 0;
static volatile int  keybuf_tail = 0;

static const char kc_map[128] = {
    0,  27,  '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5',
    '6','+','1','2','3','0','.'
};

static const char kc_map_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5',
    '6','+','1','2','3','0','.'
};

static volatile int shift_pressed = 0;
static volatile int ctrl_pressed  = 0;
static volatile int alt_pressed   = 0;

void keyboard_irq_handler(int_frame_t* frame, void* data) {
    (void)frame;
    (void)data;

    uint8_t scancode = inb(PS2_DATA);
    int released = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    if (key == 0x2A || key == 0x36) {
        shift_pressed = !released;
        return;
    }
    if (key == 0x1D) {
        ctrl_pressed = !released;
        return;
    }
    if (key == 0x38) {
        alt_pressed = !released;
        return;
    }

    if (released) return;

    char c;
    if (shift_pressed)
        c = kc_map_shift[key];
    else
        c = kc_map[key];

    if (c) {
        int next = (keybuf_head + 1) % KEYBUF_SIZE;
        if (next != keybuf_tail) {
            keybuf[keybuf_head] = c;
            keybuf_head = next;
        }
    }
}

void keyboard_init(void) {
    keybuf_head = keybuf_tail = 0;

    uint8_t status = inb(PS2_STATUS);
    (void)status;

    do {
        if (inb(PS2_STATUS) & 1)
            inb(PS2_DATA);
    } while (inb(PS2_STATUS) & 1);

    hal_irq_register(KB_IRQ, keyboard_irq_handler, NULL);

    kprintf("[KEYBOARD] PS/2 keyboard driver initialized on IRQ1\n");
}

int keyboard_data_available(void) {
    return (keybuf_head != keybuf_tail) ? 1 : 0;
}

int keyboard_getchar(void) {
    while (keybuf_head == keybuf_tail) {
        asm volatile("sti; hlt; cli");
    }
    char c = keybuf[keybuf_tail];
    keybuf_tail = (keybuf_tail + 1) % KEYBUF_SIZE;
    return c;
}
