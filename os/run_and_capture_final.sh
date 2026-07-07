#!/bin/bash
set -e
cd /home/daksh/working/OPERtur/TRY1/os
rm -f boot_full.log
echo "Running QEMU..."
timeout 180 qemu-system-x86_64 -kernel build/kernel.elf -nographic -m 512 -no-reboot -no-shutdown -nic user,model=e1000 -serial mon:stdio 2>&1 | tee boot_full.log
echo "Done. Boot log saved to boot_full.log"
