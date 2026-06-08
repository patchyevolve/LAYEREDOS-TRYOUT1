# OPERtur/TRY1 OS Build System
# ============================

TOOLCHAIN_PREFIX ?=
CC  = $(TOOLCHAIN_PREFIX)gcc
LD  = $(TOOLCHAIN_PREFIX)ld
AS  = $(CC)
OBJCOPY = $(TOOLCHAIN_PREFIX)objcopy
OBJDUMP = $(TOOLCHAIN_PREFIX)objdump
SIZE    = $(TOOLCHAIN_PREFIX)size
AR      = $(TOOLCHAIN_PREFIX)ar
QEMU    = qemu-system-x86_64

# Build configuration
BUILD ?= debug
DEBUG ?= $(shell test "$(BUILD)" = "debug" && echo 1 || echo 0)

# Feature flags
ENABLE_SMP    ?= 0
ENABLE_ACPI   ?= 0
ENABLE_NET    ?= 0

# Kernel compilation flags
KERNEL_CFLAGS = -ffreestanding -m64 -mcmodel=kernel -mno-red-zone \
                -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector \
                -fno-pic -fno-pie -nostdlib -fno-strict-aliasing \
                -fno-omit-frame-pointer

# User-space compilation flags (small model — user programs at 0x40000000)
USER_CFLAGS = -ffreestanding -m64 -mcmodel=small -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector \
              -fno-pic -fno-pie -nostdlib -fno-builtin \
              -fno-strict-aliasing -fno-omit-frame-pointer

INCLUDES = -I src/include -I src/kernel
WARNINGS = -Wall -Wextra -Werror -Wno-unused-parameter

CFLAGS  = $(KERNEL_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g
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
USER_PROGRAMS   = user_program cat_program hello-c hello-dyn
USER_ELF_BINS   = $(addprefix $(BUILD_DIR)/, $(addsuffix .elf, $(USER_PROGRAMS)))
USER_EMBEDDED   = $(addsuffix _embedded.o, $(basename $(USER_ELF_BINS)))
DYNSO_BINS      = $(BUILD_DIR)/ld.so $(BUILD_DIR)/lib/ldso/libdyn.so
DYNSO_EMBEDDED  = $(BUILD_DIR)/ld_so_embedded.o $(BUILD_DIR)/libdyn_so_embedded.o

# ---- libc sources (user-space library) ----
# Exclude the old user.c which has been replaced by the new libc
LIBC_C_SRCS = $(filter-out $(SRC_DIR)/lib/libuser/user.c, \
               $(wildcard $(SRC_DIR)/lib/libuser/*.c))
LIBC_S_SRCS = $(wildcard $(SRC_DIR)/lib/libuser/*.S)
LIBC_OBJS   = $(patsubst $(SRC_DIR)/lib/libuser/%.c, $(BUILD_DIR)/lib/libuser/%.o, $(LIBC_C_SRCS)) \
              $(patsubst $(SRC_DIR)/lib/libuser/%.S, $(BUILD_DIR)/lib/libuser/%.o, $(LIBC_S_SRCS))
LIBC_A      = $(BUILD_DIR)/libuser/libc.a

# ---- Final link objects (kernel only - user lib is for user-space programs) ----
LINK_OBJS = $(KERNEL_OBJS) $(USER_EMBEDDED) $(DYNSO_EMBEDDED)
KERNEL_ELF = $(BUILD_DIR)/kernel.elf

.PHONY: all clean rebuild run headless debug monitor dump size \
        user_programs libc ldso dynso help test

all: $(KERNEL_ELF) $(LIBC_A) $(DYNSO_BINS)

# ============================================================
# Generic pattern rules (applies to kernel AND boot .S / kernel-only .c)
# ============================================================

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# User C programs in src/boot/ use user-space flags (override generic pattern)
$(BUILD_DIR)/boot/%-c.o: $(SRC_DIR)/boot/%-c.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

# ============================================================
# libc (user-space library)
# ============================================================

# Compile libc .c files
$(BUILD_DIR)/lib/libuser/%.o: $(SRC_DIR)/lib/libuser/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

# Compile libc .S files
$(BUILD_DIR)/lib/libuser/%.o: $(SRC_DIR)/lib/libuser/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

# Archive into libc.a
$(LIBC_A): $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

libc: $(LIBC_A)

# ============================================================
# User ELF programs
# ============================================================

# Link user program ELF at fixed address 0x40000000
# Assembly-only programs (no libc):
$(BUILD_DIR)/%.elf: $(BUILD_DIR)/boot/%.o
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -Ttext=0x40000000 -o $@ $<

# C programs that use libc (program name must have -c suffix, e.g. hello-c.elf):
$(BUILD_DIR)/%-c.elf: $(BUILD_DIR)/boot/%-c.o $(LIBC_A)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -Ttext=0x40000000 -o $@ $(BUILD_DIR)/lib/libuser/crt0.o $< -L $(BUILD_DIR)/libuser -lc

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
	@echo "=== Headless automated test (Stage 3 FS) ==="
	@(sleep 8; \
	  echo "echo STAGE3:FILE_RW"; sleep 1; \
	  echo "echo hello > /test.txt"; sleep 1; \
	  echo "cat /test.txt"; sleep 1; \
	  echo "echo world >> /test.txt"; sleep 1; \
	  echo "cat /test.txt"; sleep 1; \
	  echo "wc /test.txt"; sleep 1; \
	  echo "echo STAGE3:DIR_OPS"; sleep 1; \
	  echo "mkdir /mydir"; sleep 1; \
	  echo "ls /mydir"; sleep 1; \
	  echo "rmdir /mydir"; sleep 1; \
	  echo "echo STAGE3:SYMLINK"; sleep 1; \
	  echo "ln -s /test.txt /link.txt"; sleep 1; \
	  echo "cat /link.txt"; sleep 1; \
	  echo "readlink /link.txt"; sleep 1; \
	  echo "echo STAGE3:HARDLINK"; sleep 1; \
	  echo "ln /test.txt /hard.txt"; sleep 1; \
	  echo "cat /hard.txt"; sleep 1; \
	  echo "echo STAGE3:RENAME"; sleep 1; \
	  echo "mv /test.txt /renamed.txt"; sleep 1; \
	  echo "cat /renamed.txt"; sleep 1; \
	  echo "echo STAGE3:COPY"; sleep 1; \
	  echo "cp /renamed.txt /copy.txt"; sleep 1; \
	  echo "cat /copy.txt"; sleep 1; \
	  echo "echo STAGE3:PERMS"; sleep 1; \
	  echo "chmod 444 /copy.txt"; sleep 1; \
	  echo "stat /copy.txt"; sleep 1; \
	  echo "chmod 644 /copy.txt"; sleep 1; \
	  echo "echo STAGE3:TMPFS"; sleep 1; \
	  echo "ls /tmp"; sleep 1; \
	  echo "echo STAGE3:DEVFS"; sleep 1; \
	  echo "cat /dev/null"; sleep 1; \
	  echo "wc /copy.txt"; sleep 1; \
	  echo "rm /renamed.txt /copy.txt /link.txt /hard.txt"; sleep 1; \
	  echo "echo STAGE3:SNAPSHOT"; sleep 1; \
	  echo "echo snap-data > /snap-test.txt"; sleep 1; \
	  echo "cat /snap-test.txt"; sleep 1; \
	  echo "snap take"; sleep 2; \
	  echo "echo changed > /snap-test.txt"; sleep 1; \
	  echo "snap rollback"; sleep 2; \
	  echo "cat /snap-test.txt"; sleep 1; \
	  echo "echo STAGE3:BACKUP"; sleep 1; \
	  echo "backup save /backup.bkp"; sleep 3; \
	  echo "rm /snap-test.txt"; sleep 1; \
	  echo "backup restore /backup.bkp"; sleep 3; \
	  echo "cat /snap-test.txt"; sleep 1; \
	  echo "echo STAGE3:FSCK"; sleep 1; \
	  echo "fsck"; sleep 2; \
	  echo "echo STAGE3:DONE"; sleep 1; \
	  echo "source /welcome.txt"; sleep 2; \
	  echo "run hello-c.elf"; sleep 6; \
	  echo "echo done") | \
	  timeout 210 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
	  -m 512M -no-reboot -no-shutdown 2>&1 | \
	  strings | grep -E '(hello|world|/test\.txt|/link\.txt|/renamed|/copy\.txt|/snap|STAGE3:|lines.*words.*chars|/tmp|/dev/null|libc test|PASS|done|OPERtur|Welcome|FSCK|clean|snap-data|rolled|BACKUP|RESTORE|backup\.bkp)'
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

# ============================================================
# Dynamic linker & shared library
# ============================================================

# Position-independent CFLAGS for shared objects
SO_CFLAGS = $(USER_CFLAGS) -fPIC

# ld.so — compiled with -fPIE so internal refs are RIP-relative
LDSO_CFLAGS = $(SO_CFLAGS) -fPIE -fvisibility=hidden

$(BUILD_DIR)/lib/ldso/ldso.o: $(SRC_DIR)/lib/ldso/ldso.c
	@mkdir -p $(dir $@)
	$(CC) $(LDSO_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

# Link as ET_DYN (position-independent executable) with vaddr=0
$(BUILD_DIR)/ld.so: $(BUILD_DIR)/lib/ldso/ldso.o
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -pie --no-dynamic-linker -z max-page-size=0x1000 -o $@ $<

# libdyn.so — test shared library (export all symbols)
$(BUILD_DIR)/lib/ldso/libdyn.o: $(SRC_DIR)/lib/ldso/libdyn.c
	@mkdir -p $(dir $@)
	$(CC) $(SO_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

$(BUILD_DIR)/lib/ldso/libdyn.so: $(BUILD_DIR)/lib/ldso/libdyn.o
	$(LD) -nostdlib -shared -z max-page-size=0x1000 -o $@ $<

# Copy to build root so objcopy creates _binary_build_libdyn_so_* symbols
$(BUILD_DIR)/libdyn.so: $(BUILD_DIR)/lib/ldso/libdyn.so
	cp $< $@

# hello-dyn.elf — dynamically-linked test program with PT_INTERP
$(BUILD_DIR)/boot/hello-dyn.o: $(SRC_DIR)/boot/hello-dyn.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

$(BUILD_DIR)/hello-dyn.elf: $(BUILD_DIR)/boot/hello-dyn.o $(BUILD_DIR)/libdyn.so $(LIBC_A)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -dynamic-linker /ld.so -z max-page-size=0x1000 \
		-o $@ $(BUILD_DIR)/lib/libuser/crt0.o $< \
		-L $(BUILD_DIR)/lib/ldso -ldyn \
		-L $(BUILD_DIR)/libuser -lc

# Embed ld.so and libdyn.so as binary .rodata objects
# (must be at build root so objcopy creates _binary_build_* names)
$(BUILD_DIR)/ld_so_embedded.o: $(BUILD_DIR)/ld.so
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--rename-section .data=.rodata $< $@

$(BUILD_DIR)/libdyn_so_embedded.o: $(BUILD_DIR)/libdyn.so
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--rename-section .data=.rodata $< $@

ldso: $(BUILD_DIR)/ld.so
	@echo "ld.so built"

dynso: $(DYNSO_BINS)
	@echo "Shared objects built: $(DYNSO_BINS)"

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
