#!/bin/bash
cd /home/daksh/working/OPERtur/TRY1/os
# Run QEMU and capture all output for 2 minutes, then exit
timeout -s SIGTERM 120 qemu-system-x86_64 \
    -kernel build/kernel.elf \
    -nographic \
    -m 512M \
    -no-reboot \
    -no-shutdown \
    -nic user,model=e1000 \
    -serial mon:stdio
