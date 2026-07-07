#!/bin/bash
cd /home/daksh/working/OPERtur/TRY1/os || exit 1

rm -f full_log.txt

echo "=== QEMU start ==="
timeout 240 qemu-system-x86_64 \
  -kernel build/kernel.elf \
  -nographic \
  -m 512M \
  -no-reboot \
  -no-shutdown \
  -nic user,model=e1000 \
  -serial file:full_log.txt > /dev/null 2>&1

echo
echo "=== Full QEMU output ==="
cat full_log.txt
