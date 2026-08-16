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
RELEASE ?= $(shell test "$(BUILD)" = "release" && echo 1 || echo 0)

# Feature flags
ENABLE_SMP       ?= 1
ENABLE_ACPI      ?= 1
ENABLE_NET       ?= 0
# Disable QEMU SMP quirk workaround (enable SMP even with PCI network device)
ENABLE_QEMU_SMP_WITH_NIC ?= 0
ENABLE_NET_TEST       ?= 0
ENABLE_STORAGE_TEST   ?= 0
ENABLE_KERNEL_TEST    ?= 0
ENABLE_SFS_TEST       ?= 0
ENABLE_PROCESS_TEST   ?= 0
ENABLE_SECURITY_TEST  ?= 0
ENABLE_LOCKDEP        ?= 0

# Kernel compilation flags
KERNEL_CFLAGS = -ffreestanding -m64 -mcmodel=kernel -mno-red-zone \
                -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector \
                -fno-pic -fno-pie -nostdlib -fno-strict-aliasing

# User-space compilation flags (position-independent for ASLR)
USER_CFLAGS = -ffreestanding -m64 -mcmodel=small -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector \
              -fPIE -fPIC -nostdlib -fno-builtin \
              -fno-strict-aliasing

INCLUDES = -I src/include -I src/kernel -I src/libtls -I $(BUILD_DIR)/kernel
WARNINGS = -Wall -Wextra -Werror -Wno-unused-parameter

# Debug vs release flags
ifeq ($(RELEASE),1)
KERNEL_CFLAGS += -fomit-frame-pointer
USER_CFLAGS   += -fomit-frame-pointer
OPT_FLAGS      = -Os -DNDEBUG
POST_LD        = $(OBJCOPY) --strip-all --discard-all build/kernel.elf build/kernel.elf
else
KERNEL_CFLAGS += -fno-omit-frame-pointer
USER_CFLAGS   += -fno-omit-frame-pointer
OPT_FLAGS      = -O2 -g
POST_LD        = 
endif

CFLAGS  = $(KERNEL_CFLAGS) $(INCLUDES) $(WARNINGS) $(OPT_FLAGS)
CFLAGS  += $(shell [ $(ENABLE_SMP) -eq 1 ] && echo "-DCONFIG_SMP")
CFLAGS  += $(shell [ $(ENABLE_ACPI) -eq 1 ] && echo "-DCONFIG_ACPI")
CFLAGS  += $(shell [ $(ENABLE_QEMU_SMP_WITH_NIC) -eq 1 ] && echo "-DCONFIG_QEMU_SMP_WITH_NIC")
CFLAGS  += $(shell [ $(ENABLE_NET_TEST) -eq 1 ] && echo "-DNET_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_STORAGE_TEST) -eq 1 ] && echo "-DSTORAGE_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_KERNEL_TEST) -eq 1 ] && echo "-DKERNEL_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_SFS_TEST) -eq 1 ] && echo "-DSFS_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_PROCESS_TEST) -eq 1 ] && echo "-DPROCESS_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_SECURITY_TEST) -eq 1 ] && echo "-DSECURITY_SELF_TEST")
CFLAGS  += $(shell [ $(ENABLE_LOCKDEP) -eq 1 ] && echo "-DCONFIG_LOCKDEP")
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T linker.ld

LIBGCC := $(shell $(CC) -print-libgcc-file-name 2>/dev/null)

SRC_DIR   = src
BUILD_DIR = build

# ---- Kernel sources (exclude user programs and user library) ----
KERNEL_C_SRCS = $(filter-out $(SRC_DIR)/kernel/net_test.c $(SRC_DIR)/kernel/storage_test.c $(SRC_DIR)/kernel/kernel_test.c $(SRC_DIR)/kernel/sfs_test.c $(SRC_DIR)/kernel/process_test.c $(SRC_DIR)/kernel/security_test.c, $(wildcard $(SRC_DIR)/kernel/*.c)) \
                $(wildcard $(SRC_DIR)/lib/*.c)
ifeq ($(ENABLE_NET_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/net_test.c
endif
ifeq ($(ENABLE_STORAGE_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/storage_test.c
endif
ifeq ($(ENABLE_KERNEL_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/kernel_test.c
endif
ifeq ($(ENABLE_SFS_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/sfs_test.c
endif
ifeq ($(ENABLE_PROCESS_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/process_test.c
endif
ifeq ($(ENABLE_SECURITY_TEST),1)
KERNEL_C_SRCS += $(SRC_DIR)/kernel/security_test.c
endif
KERNEL_S_SRCS = $(filter-out \
                    $(SRC_DIR)/boot/user_program.S \
                    $(SRC_DIR)/boot/cat_program.S \
                    $(SRC_DIR)/boot/minimal_test.S \
                    $(SRC_DIR)/boot/exit_only.S, \
                    $(wildcard $(SRC_DIR)/boot/*.S)) \
                $(wildcard $(SRC_DIR)/kernel/*.S)

KERNEL_C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(KERNEL_C_SRCS))
KERNEL_S_OBJS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(KERNEL_S_SRCS))
KERNEL_OBJS   = $(KERNEL_C_OBJS) $(KERNEL_S_OBJS)

# ---- User ELF binaries (embedded as .rodata in kernel) ----
USER_PROGRAMS   = user_program cat_program hello-c hello-dyn tcp_echo-c udp_echo-c exit_only thread_test-c https_get-c netns_mini-c minimal_test
USER_ELF_BINS   = $(addprefix $(BUILD_DIR)/, $(addsuffix .elf, $(USER_PROGRAMS)))
USER_EMBEDDED   = $(addsuffix _embedded.o, $(basename $(USER_ELF_BINS)))
DYNSO_BINS      = $(BUILD_DIR)/ld.so $(BUILD_DIR)/lib/ldso/libdyn.so
DYNSO_EMBEDDED  = $(BUILD_DIR)/ld_so_embedded.o $(BUILD_DIR)/libdyn_so_embedded.o

# ---- Secure boot: auto-generated hash table of known-good binaries ----
SECURE_BOOT_HASHES_C = $(BUILD_DIR)/kernel/secure_boot_hashes.c
SECURE_BOOT_ELFS     = $(USER_ELF_BINS) $(DYNSO_BINS)

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

all: $(KERNEL_ELF) $(LIBC_A) $(LTLSA) $(DYNSO_BINS)

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

# Compile libuser .S files
$(BUILD_DIR)/lib/libuser/%.o: $(SRC_DIR)/lib/libuser/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

# Archive into libc.a
$(LIBC_A): $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

libc: $(LIBC_A)

# ============================================================
# libtls (user-space TLS library)
# ============================================================
LIBSRC_LIBTLS = $(SRC_DIR)/libtls
LTLSSRCS = $(wildcard $(LIBSRC_LIBTLS)/*.c)
LTLSOBJS = $(patsubst $(LIBSRC_LIBTLS)/%.c, $(BUILD_DIR)/libtls/%.o, $(LTLSSRCS))
LTLSA    = $(BUILD_DIR)/libuser/libtls.a

$(BUILD_DIR)/libtls/%.o: $(LIBSRC_LIBTLS)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -c $< -o $@

$(LTLSA): $(LTLSOBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

libtls: $(LTLSA)

# ============================================================
# User ELF programs
# ============================================================

# Link user programs as PIE (ET_DYN) so kernel applies ASLR
# Assembly-only programs (no libc) — static PIE, no interpreter:
$(BUILD_DIR)/%.elf: $(BUILD_DIR)/boot/%.o
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -pie --no-dynamic-linker -z max-page-size=0x1000 -o $@ $<

# C programs that use libc — static PIE, no interpreter:
$(BUILD_DIR)/%-c.elf: $(BUILD_DIR)/boot/%-c.o $(LIBC_A) $(LTLSA)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -pie --no-dynamic-linker -z max-page-size=0x1000 -o $@ $(BUILD_DIR)/lib/libuser/crt0.o $< -L $(BUILD_DIR)/libuser -lc -ltls

# Embed as binary .rodata object (kernel links this)
$(BUILD_DIR)/%_embedded.o: $(BUILD_DIR)/%.elf
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--rename-section .data=.rodata $< $@

# ============================================================
# Kernel link
# ============================================================

# Generate secure boot hash table from all known-good ELF binaries
$(SECURE_BOOT_HASHES_C): $(SECURE_BOOT_ELFS) scripts/gen_secure_boot_hashes.py
	@mkdir -p $(BUILD_DIR)/kernel
	python3 scripts/gen_secure_boot_hashes.py $@ $(SECURE_BOOT_ELFS)

# secure_boot.o depends on the generated hash table
$(BUILD_DIR)/kernel/secure_boot.o: $(SECURE_BOOT_HASHES_C)

$(KERNEL_ELF): $(LINK_OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(LINK_OBJS) $(LIBGCC)
	$(POST_LD)
	@$(SIZE) $@

# Build-time SFS root filesystem image
ROOT_SFS = $(BUILD_DIR)/root.sfs

$(ROOT_SFS): $(SECURE_BOOT_ELFS) tools/mksfs.py
	@echo "[SFS] Building root filesystem image..."
	@python3 tools/mksfs.py
	@echo "[SFS] Root filesystem: $(ROOT_SFS)"

# ============================================================
# Convenience targets
# ============================================================

run: clean all
	$(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic user,model=e1000

headless: clean all
	$(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-d cpu_reset -D qemu.log

test: $(KERNEL_ELF)
	@echo "=== Headless automated test (Stage 3 FS) ==="
	@(sleep 60; \
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
	  echo "run thread_test.elf"; sleep 6; \
	  echo "echo done") | \
	  timeout 300 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
	  -m 512M -no-reboot -no-shutdown 2>&1 | \
	  strings | grep -E '(hello|world|/test\.txt|/link\.txt|/renamed|/copy\.txt|/snap|STAGE3:|lines.*words.*chars|/tmp|/dev/null|libc test|threading|fork_test|mmap_test|thread_test|PASS|done|OPERtur|Welcome|FSCK|clean|snap-data|rolled|BACKUP|RESTORE|backup\.bkp)'
	@echo "=== Test complete ==="

release:
	$(MAKE) BUILD=release all

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

# Position-independent CFLAGS for shared objects (USER_CFLAGS already has -fPIC)
SO_CFLAGS = $(USER_CFLAGS)

# ld.so — compiled with -fPIE so internal refs are RIP-relative
LDSO_CFLAGS = $(SO_CFLAGS) -fPIE -fvisibility=hidden -fno-stack-protector

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
	$(CC) $(SO_CFLAGS) $(INCLUDES) $(WARNINGS) -O2 -g -fno-stack-protector -c $< -o $@

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
	$(LD) -nostdlib -pie -dynamic-linker /ld.so -z max-page-size=0x1000 \
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
	@echo "  test-security - Run security self-tests"
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
	rm -f $(SECURE_BOOT_HASHES_C)

test-net:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_NET_TEST=1 all
	@echo "Running network self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_net.txt 2>&1 || true
	@grep -a 'tcp_find_conn_ipv6\|ndp_cache_miss\|icmpv6_ns_parse\|socket_refcount\|udp_queue\|Results:' /tmp/qemu_test_net.txt || true

test-storage:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_STORAGE_TEST=1 all
	@echo "Running storage self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_storage.txt 2>&1 || true
	@grep -a 'block_cache_basic\|block_cache_writeback\|block_register_reject\|swap_alloc_free\|swap_out_in\|journal_init\|journal_log_commit\|snapshot_take_info\|Results:' /tmp/qemu_test_storage.txt || true

test-kernel:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_KERNEL_TEST=1 all
	@echo "Running kernel self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_kernel.txt 2>&1 || true
	@grep -a 'kmalloc_free\|kmalloc_edge\|vma_add_find\|tcp_conn_create\|tcp_conn_bind_listen\|socket_lifecycle\|spinlock_acquire\|tcp_retransmission\|mutex_lock\|udp_endpoint_multi\|block_cache_eviction\|rwlock_basic\|seqlock_basic\|lockdep_ordering\|ata_concurrent\|kmag_concurrent\|ap_timer\|watchdog_lockup\|sched_steal\|sched_balance_push\|sched_affinity_pin\|Results:' /tmp/qemu_test_kernel.txt || true

test-sfs:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_SFS_TEST=1 all
	@echo "Running SFS self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_sfs.txt 2>&1 || true
	@grep -a 'sfs_create_write\|sfs_mkdir_and\|sfs_rename\|sfs_links\|sfs_stat\|Results:' /tmp/qemu_test_sfs.txt || true

test-process:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_PROCESS_TEST=1 all
	@echo "Running process self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_process.txt 2>&1 || true
	@grep -a 'process_create_find\|process_exit_code\|process_fork_basic\|process_zombie\|Results:' /tmp/qemu_test_process.txt || true

test-security:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_SECURITY_TEST=1 all
	@echo "Running security self-tests..."
	@timeout 120 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_security.txt 2>&1 || true
	@grep -a 'syscall_bad_fd\|syscall_null_buf\|user_ptr_checks\|file_permissions\|process_memory_isolation\|stack_canary\|vfs_fd_mode_enforcement\|Results:' /tmp/qemu_test_security.txt || true

ISO_DIR = iso

rootfs: $(ROOT_SFS)

iso: $(KERNEL_ELF)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	@if command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue -o opertur.iso $(ISO_DIR) && \
		rm -rf $(ISO_DIR) && \
		echo "[ISO] Created opertur.iso"; \
	else \
		echo "[ISO] grub-mkrescue not found."; \
		echo "  Run: sudo apt install grub-pc-bin xorriso"; \
		echo "  Then: grub-mkrescue -o opertur.iso $(ISO_DIR)"; \
	fi

test-all:
	$(MAKE) clean >/dev/null 2>&1
	$(MAKE) ENABLE_NET_TEST=1 ENABLE_STORAGE_TEST=1 ENABLE_KERNEL_TEST=1 ENABLE_SFS_TEST=1 ENABLE_PROCESS_TEST=1 ENABLE_SECURITY_TEST=1 all
	@echo "Running all self-tests (one boot)..."
	@timeout 180 $(QEMU) -kernel $(KERNEL_ELF) -nographic \
		-m 512M -no-reboot -no-shutdown \
		-nic none -device e1000 > /tmp/qemu_test_all.txt 2>&1 || true
	@grep -a 'PASS\|FAIL\|Results:' /tmp/qemu_test_all.txt || true

test-net-2qemu: all
	@QEMU="$(QEMU)" chmod +x test-2qemu.sh; \
	 QEMU="$(QEMU)" ./test-2qemu.sh
