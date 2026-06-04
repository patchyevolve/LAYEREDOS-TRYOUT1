ENTRY(start)

KERNEL_PHYS_BASE = 0x100000;
KERNEL_VMA_BASE = 0xFFFFFFFFC0000000;

SECTIONS {
    . = KERNEL_PHYS_BASE;

    .multiboot : {
        *(.multiboot)
    }

    .boot_text : {
        *(.boot.text)
    }

    .boot_data : {
        *(.boot.data)
    }

    .boot_notes : {
        *(.note.pvh)
    }

    . += KERNEL_VMA_BASE;

    .text : AT(ADDR(.text) - KERNEL_VMA_BASE) {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA_BASE) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VMA_BASE) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    .bss : AT(ADDR(.bss) - KERNEL_VMA_BASE) {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        _bss_end = .;
    }

    _kernel_end = .;
    _kernel_end_phys = _kernel_end - KERNEL_VMA_BASE;
}
