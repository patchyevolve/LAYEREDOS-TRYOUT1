.section .multiboot, "a", @progbits
.align 8
multiboot_header_start:
.long 0xE85250D6              /* multiboot2 magic */
.long 0                        /* architecture: 0 = i386 */
.long multiboot_header_end - multiboot_header_start  /* header length */
.long -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header_start))  /* checksum */

/* End tag (required) */
.word 0    /* type = 0 */
.word 0    /* flags */
.long 8    /* size */
multiboot_header_end:
