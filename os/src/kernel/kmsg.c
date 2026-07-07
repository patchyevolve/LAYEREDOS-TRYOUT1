#include "kernel.h"
#include "kmsg.h"

#define KMSG_BUF_SIZE 4096
#define KMSG_MASK     (KMSG_BUF_SIZE - 1)

static char kmsg_buf[KMSG_BUF_SIZE];
static volatile uint32_t kmsg_head;  /* producer index */
static volatile uint32_t kmsg_tail;  /* consumer index */

void kmsg_putchar(char c) {
    uint32_t next = (kmsg_head + 1) & KMSG_MASK;
    if (next == kmsg_tail) {
        /* Buffer full: advance tail to drop oldest char */
        kmsg_tail = (kmsg_tail + 1) & KMSG_MASK;
    }
    kmsg_buf[kmsg_head] = c;
    kmsg_head = next;
}

void kmsg_dump(void) {
    kprintf("\n====== KMSG DUMP ======\n");
    uint32_t t = kmsg_tail;
    while (t != kmsg_head) {
        char c = kmsg_buf[t];
        if (c == '\n') {
            kputchar('\r');
            kputchar('\n');
        } else if (c >= ' ') {
            kputchar(c);
        }
        t = (t + 1) & KMSG_MASK;
    }
    kprintf("\n========================\n");
}
