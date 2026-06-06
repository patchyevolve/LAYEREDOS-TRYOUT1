# OPERtur/TRY1 OS Build System
# ============================

TOOLCHAIN_PREFIX ?=
CC  = $(TOOLCHAIN_PREFIX)gcc
LD  = $(TOOLCHAIN_PREFIX)ld
AS  = $(CC)
OBJCOPY = $(TOOLCHAIN_PREFIX)objcopy
OBJDUMP = $(TOOLCHAIN_PREFIX)objdump
SIZE    = $(TOOLCHAIN_PREFIX)size
QEMU    = qemu-system-x86_64

# Build configuration
BUILD ?= debug
DEBUG ?= $(shell test "$(BUILD)" = "debug" && echo 1 || echo 0)

# Feature flags
ENABLE_SMP    ?= 0
ENABLE_ACPI   ?= 0
ENABLE_NET    ?= 0

# Common flags
ARCH_CFLAGS = -ffreestanding -m64 -mcmodel=kernel -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector \
              -fno-pic -fno-pie -nostdlib -fno-strict-aliasing \
              -fno-omit-frame-pointer

INCLUDES = -I src/include -I src/kernel
WARNINGS = -Wall -Wextra -Werror -Wno-unused-parameter

CFLAGS  = $(ARCH_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T linker.ld

LIBGCC := $(shell $(CC) -print-libgcc-file-name 2>/dev/null)

SRC_DIR   = src
BUILD_DIR = build

# ---- Kernel sources (exclude user programs and user library) ----
KERNEL_C_SRCS = $(wildcard $(SRC_DIR)/kernel/*.c) \
                $(wildcard $(SRC_DIR)/lib/*.c)
KERNEL_S_SRCS = $(filter-out \
                    $(SRC_DIR)/boot/user_program.S \
                    $(SRC_DIR)/boot/cat_program.S, \
                    $(wildcard $(SRC_DIR)/boot/*.S)) \
                $(wildcard $(SRC_DIR)/kernel/*.S)

KERNEL_C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(KERNEL_C_SRCS))
KERNEL_S_OBJS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(KERNEL_S_SRCS))
KERNEL_OBJS   = $(KERNEL_C_OBJS) $(KERNEL_S_OBJS)

# ---- User ELF binaries (embedded as .rodata in kernel) ----
USER_PROGRAMS   = user_program cat_program
USER_ELF_BINS   = $(addprefix $(BUILD_DIR)/, $(addsuffix .elf, $(USER_PROGRAMS)))
USER_EMBEDDED   = $(addsuffix _embedded.o, $(basename $(USER_ELF_BINS)))

# ---- User-space library (linked into kernel for now) ----
USER_LIB_C_SRCS = $(wildcard $(SRC_DIR)/lib/libuser/*.c)
USER_LIB_S_SRCS = $(wildcard $(SRC_DIR)/lib/libuser/*.S)

# ---- Final link objects (kernel only - user lib is for user-space programs) ----
LINK_OBJS = $(KERNEL_OBJS) $(USER_EMBEDDED)
KERNEL_ELF = $(BUILD_DIR)/kernel.elf

.PHONY: all clean rebuild run headless debug monitor dump size \
        user_programs help test

all: $(KERNEL_ELF)

# ============================================================
# Generic pattern rules
# ============================================================

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# ============================================================
# User ELF programs
# ============================================================

# Link user program ELF at fixed address 0x40000000
$(BUILD_DIR)/%.elf: $(BUILD_DIR)/boot/%.o
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -Ttext=0x40000000 -o $@ $<

# Embed as binary .rodata object (kernel links this)
$(BUILD_DIR)/%_embedded.o: $(BUILD_DIR)/%.elf
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--rename-section .data=.rodata $< $@

# ============================================================
# Kernel link
# ============================================================

$(KERNEL_ELF): $(LINK_OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(LINK_OBJS) $(LIBGCC)
	@$(SIZE) $@

# ============================================================
# Convenience targets
# ============================================================

run: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial mon:stdio \
		-m 512M -no-reboot -no-shutdown -vga std

headless: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-d cpu_reset -D qemu.log

test: $(KERNEL_ELF)
	@echo "=== Headless automated test ==="
	@(sleep 4; echo "ls"; sleep 1; echo "touch /testfile.txt"; sleep 1; \
	  echo "ls"; sleep 1; echo "rm /testfile.txt"; sleep 1; \
	  echo "ls"; sleep 1; echo "type ls"; sleep 1; \
	  echo "alias ll=ls"; sleep 1; echo "type ll"; sleep 1; \
	  echo "source /welcome.txt"; sleep 2; \
	  echo "echo done") | \
	  timeout 22 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
	  -m 512M -no-reboot -no-shutdown 2>&1 | \
	  strings | grep -E '(OS>|Hello|OPERtur|Layered|Welcome|testfile|built-in|aliased|not found|echo done)'
	@echo "=== Test complete ==="

debug: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial mon:stdio \
		-m 512M -no-reboot -no-shutdown -vga std \
		-gdb tcp::1234 -S -d int,cpu_reset -D qemu.log

monitor: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial mon:stdio \
		-m 512M -no-reboot -no-shutdown -vga std -monitor vc

dump: $(KERNEL_ELF)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(BUILD_DIR)/kernel.bin
	@echo "Kernel binary dumped to $(BUILD_DIR)/kernel.bin"
	@echo "Size: $$(wc -c < $(BUILD_DIR)/kernel.bin) bytes"

size: $(KERNEL_ELF)
	@echo "=== Section Sizes ==="
	$(SIZE) -A $(KERNEL_ELF)
	@echo ""
	@echo "Total ELF size: $$(wc -c < $(KERNEL_ELF)) bytes"

user_programs: $(USER_ELF_BINS)
	@echo "User programs built: $(USER_ELF_BINS)"

help:
	@echo "OPERtur/TRY1 OS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build kernel (default)"
	@echo "  run       - Build and run in QEMU"
	@echo "  headless  - Run in headless mode (serial only)"
	@echo "  test      - Run headless automated test"
	@echo "  debug     - Run with GDB stub on :1234"
	@echo "  monitor   - Run with QEMU monitor"
	@echo "  dump      - Dump raw kernel binary"
	@echo "  size      - Show kernel section sizes"
	@echo "  clean     - Remove build artifacts"
	@echo "  rebuild   - Clean and rebuild"
	@echo "  user_programs - Build user ELF binaries"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD=debug|release  (default: debug)"
	@echo "  TOOLCHAIN_PREFIX=    (e.g. x86_64-elf-)"

rebuild: clean all

clean:
	rm -rf $(BUILD_DIR)
	rm -f qemu.log
