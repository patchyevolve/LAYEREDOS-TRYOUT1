CC = gcc
LD = ld
AS = gcc
OBJCOPY = objcopy
QEMU = qemu-system-x86_64

CFLAGS = -ffreestanding -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -fno-pic -fno-pie -nostdlib \
         -I src/include \
         -Wall -Wextra -Werror -Wno-unused-parameter \
         -O2 -g

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T linker.ld

SRC_DIR = src
BUILD_DIR = build

C_SRCS = $(wildcard $(SRC_DIR)/boot/*.c) \
         $(wildcard $(SRC_DIR)/kernel/*.c) \
         $(wildcard $(SRC_DIR)/lib/*.c)
S_SRCS = $(wildcard $(SRC_DIR)/boot/*.S) \
         $(wildcard $(SRC_DIR)/kernel/*.S)

C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
S_OBJS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SRCS))
OBJS = $(C_OBJS) $(S_OBJS)

KERNEL_ELF = $(BUILD_DIR)/kernel.elf

.PHONY: all clean run debug

all: $(KERNEL_ELF)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

run: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial stdio \
		-m 512M -no-reboot -no-shutdown \
		-d cpu_reset -D qemu.log

debug: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial stdio \
		-m 512M -no-reboot -no-shutdown \
		-gdb tcp::1234 -S -d int,cpu_reset -D qemu.log

monitor: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -serial stdio \
		-m 512M -no-reboot -no-shutdown -monitor stdio

dump: $(KERNEL_ELF)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(BUILD_DIR)/kernel.bin
	@echo "Kernel binary dumped to $(BUILD_DIR)/kernel.bin"
	@echo "Size: $$(wc -c < $(BUILD_DIR)/kernel.bin) bytes"

size: $(KERNEL_ELF)
	@echo "=== Section Sizes ==="
	@$(OBJCOPY) -O binary $(KERNEL_ELF) /dev/null 2>&1 || true
	@size -A $(KERNEL_ELF)
	@echo ""
	@echo "Total ELF size: $$(wc -c < $(KERNEL_ELF)) bytes"

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all
