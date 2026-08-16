# Bare-Metal Boot Plan

Priority-ordered phases for booting OPERtur on real x86-64 hardware.

## Phase 1: Must-Have for First Boot (~2 days)

### 1.1 VGA text-mode console
Real PCs don't have a serial port accessible without an adapter (laptops especially). VGA text mode at `0xB8000` is universal on BIOS-booted x86-64.

- Wrap `kprintf` to write to both serial + VGA framebuffer.
- Track cursor position (row/col), handle scrolling.
- ~80 lines, no dependencies.

### 1.2 PS/2 keyboard ✓ (already done)

### 1.3 GRUB/ISO image for real boot
Currently boots via QEMU `-kernel` which uses QEMU-internal Multiboot loading. On real hardware, need `grub-mkrescue` with a `grub.cfg`.

- Create `os/boot/grub.cfg` with `multiboot2 /kernel.elf`
- Add `make iso` target: `grub-mkrescue -o opertur.iso build/`
- ~10 lines Makefile + 5 lines grub.cfg.

### 1.4 Reset reason / ACPI reboot check
`panic_reboot()` uses triple-fault or keyboard controller `0x64` reset. Verify ACPI `reset_reg` from FADT for reliable reboot on real PCs.

## Phase 2: Boot from Disk (~3 days)

### 2.1 Root device discovery
Scan GPT partitions for one with an SFS superblock signature. Mount it as `/` instead of the embedded ramdisk.

### 2.2 Move embedded ELFs to disk SFS
Build-time: create an SFS image file containing all embedded ELF programs. The kernel then reads from the real SFS partition at boot instead of copying from embedded blobs.

### 2.3 Remove ramdisk dependency
Once disk-boot works, the 2 MB ramdisk becomes optional (fallback only).
**DONE (2026-08-04)**: disk SFS images carry all 11 boot files (mksfs.py); the kernel skips the embedded-blob copy on disk boot; ramdisk remains as fallback only. Also fixed a latent `mksfs.py` bug where chunked 7-entry groups were padded to block size, breaking dirent contiguity (entries 7+ were invisible).

## Phase 3: Networking on Real Hardware (~1 week each)

### 3.1 Intel I219/I210 (e1000e/igb)
The E1000 driver exists but only supports 82540EM/82545EM (QEMU defaults). Modern Intel consumer boards have I219-V/I210.

- PCIe, different descriptor layout (legacy Rx/Tx descriptors are similar).
- Can reuse ~60% of existing e1000.c.
- Objective: DHCP lease + TCP echo on a real PC.
- **PARTIAL (2026-08-16, QEMU-validated only — real PC not yet tested)**: driver now supports the `e1000e` (82574L) and `igb` (82576) families via a model table (devid → family → feature flags: advanced descriptors, TIPG/paperback, per-queue TXDCTL0/RXDCTL0, MDIC autoneg restart). Advanced TX/RX descriptor layouts corrected (buffer_addr@0 / cmd_type_len@8 / olinfo_status@12; pkt_addr@0 / status_error@8 u32 / length@12 / vlan@14). QEMU MDIC OP bits are **swapped vs. Intel silicon** (`OP_WRITE=0x04000000`, `OP_READ=0x08000000` in QEMU; Intel is the reverse) — driver selects via `hal_is_qemu()`; on QEMU, igb link-up additionally requires a guest MDIC BMCR.ANRESTART (a guest soft reset cancels the model's autoneg timer permanently). Verified on QEMU: link up + full DHCP DORA on igb/e1000e/e1000 (KVM, `-netdev user`). **The Phase 3.1 objective (DHCP lease + TCP echo on a real PC) is NOT yet met — real hardware untested.** Real-hardware caveats for the next bare-metal session: (1) I219-V/I210 PHYs (K1/I217 etc.) use different MDI settings and may need `PHY_CTRL`/`KMRN` config beyond ANRESTART — autoneg will likely need per-PHY tuning; (2) igb 82575/82576 can be backed by 82575-compatible PHYs (e.g. M88); (3) e1000e reset type and `SMBus`/`LAN_PWR_CTRL` matters on some laptops; (4) MSI/MSI-X not yet used (3.4).

### 3.2 RTL8169 (Realtek)
Very common on consumer motherboards. Bigger register-level difference from E1000.

### 3.3 PCIe ECAM
Currently using legacy PCI config space (`0xCF8`/`0xCFC`, 256 bytes). PCIe devices need ECAM (MMIO-based config) for extended capabilities.
- **DONE (2026-08-16, QEMU-validated)**: ACPI MCFG parsed (`acpi_parse_mcfg()`), `pci_config_read/write` dispatch to MMIO ECAM (lazy 4 KB page mapping, same pattern as `acpi_map_table`) when MCFG present, legacy `0xCF8/0xCFC` fallback otherwise. All drivers (e1000, ahci, nvme) transparently use ECAM on q35. Verified on q35: MCFG base=0xB0000000 bus 0-255, 3/3 devices identical vs legacy, e1000 + full DHCP DORA through ECAM. i440fx (no MCFG): legacy path untouched. Test `test_pci_ecam` (bidirectional legacy/ECAM equivalence, skipped when no MCFG). 89/89 tests pass.

### 3.4 MSI/MSI-X
Without MSI-X, NVMe and NICs share IRQs via IOAPIC line routing. Needed for performance and to avoid IRQ sharing issues on real hardware.

- **DONE (2026-08-17, QEMU q35-validated — real PC not yet tested)**: kernel MSI/MSI-X support:
  - `pci_find_cap()` walks the config capability chain; `pci_msix_probe()` parses the MSI-X capability (table BAR/offset/size); `pci_msix_enable()` programs every table entry (addr=`0xFEE00000|apic_id<<12`, data=vector 0x48, unmasked) and sets the MSI-X enable bit; `pci_msi_enable()` handles 64-bit MSI capabilities. New IDT stubs 72–79; dedicated vector 0x48 (irq 40) for PCI MSI-X/MSI delivery.
  - e1000e/igb driver: MSI-X probed first, MSI fallback, legacy INTx otherwise; minimal ISR (counts + acks ICR; the NIC poll thread keeps draining RX — no eth_rx_poll from ISR context). e1000e needs IVAR programmed: real 82574 wants `IVAR@0x1700 = 0x00888880` (valid bit 0x80), QEMU's model wants `IVAR@0xE4 = 0x00088888` (valid bit 0x8) — both written, each is a no-op on the other platform. e1000e raises `ICR_RXQ0` (bit 20 — NOT RXT0), which must be in IMS.
  - **QEMU quirks documented along the way**: (1) `pci_config_read()` reads the 4-byte-aligned word (`offset & 0xFC`), so the capabilities-list bit is STATUS bit 4 = bit 20 of the aligned read at 0x04, and MSI-X msgctl/IVAR-style fields at odd offsets live in the upper half of the aligned word — enable/probe code must shift accordingly and read-modify-write to preserve cap id/next bytes; (2) QEMU e1000e exposes IVAR at 0xE4 with nibble entries (valid=0x8), unlike real 82574 (0x1700, valid=0x80); (3) **hal.c CPUID bug fixed**: the "TCGTCGTCG" ebx constant had G/C swapped (`0x54434754` → `0x54474354`) and `hal_is_qemu()` never checked TCG at all — under TCG the driver silently used Intel MDIC bits (igb link never came up) and skipped the QEMU IVAR write. Both functions now check the 0x40000000 signature directly (some QEMU builds omit the hypervisor-present bit under TCG).
  - Verified: MSI-X enabled + interrupts delivered (ICR-acked, DHCP DORA completes) on e1000e and igb under **both KVM and TCG**; classic e1000 stays on legacy INTx. `test_pci_msix` (kernel_test.c): capability walk, table spec, entry-0 programming, msgctl enable, `e1000_msix_count() > 0` — full PASS on q35 KVM (8 interrupts) and TCG; SKIPPED→PASS on i440fx. 90/90 `test-all`, debug + release builds clean.
  - Real-PC notes: vector 0x48/irq 40 is free on PIC (0x20–0x2F), IPIs (0x41–0x44) and syscall 0x80. Bare-metal MSI-X should work as-is; MSI (cap at 0x50/0xD0) is a fallback. If an IOMMU (VT-d) blocks non-DMA-remapped MSIs on some platforms, a DMRR/identity-map pass would be needed (not implemented — accepted limitation).

## Phase 4: Polish (~3-5 days total)

### 4.1 USB keyboard (xHCI)
No USB support at all. Many modern laptops have no PS/2 port. xHCI driver needed for input on those systems.

### 4.2 UEFI stub
Modern UEFI-only laptops have no CSM. Need EFI handover protocol or systemd-boot-compatible stub.

### 4.3 VGA framebuffer (higher resolution)
Move from 80x25 text mode to a linear framebuffer (via GOP on UEFI or VESA on BIOS).

## Current Status

| Phase | Item | Status |
|-------|------|--------|
| 1 | 1.1 VGA text-mode console | **DONE** (exists in `klib.c` — `0xB8000` mapped via boot.S 2MB superpage) |
| 1 | 1.2 PS/2 keyboard | DONE |
| 1 | 1.3 GRUB/ISO target | **DONE** (`make iso` target + `boot/grub.cfg` multiboot2; not yet boot-tested — `grub-mkrescue`/`xorriso` not installed on dev box) |
| 1 | 1.4 ACPI FADT reset_reg for reboot | DONE (`acpi_fadt_reset()` in `acpi.c`, wired as first attempt in `hal_reboot()`) |
| 2 | 2.1 Root device discovery | **DONE** (GPT partition scan + SFS superblock probe, `gpt.c`) |
| 2 | 2.2 Move ELFs to disk SFS | **DONE** (mksfs.py image carries all boot files; disk boot skips embedded-blob copy) |
| 2 | 2.3 Remove ramdisk dependency | **DONE** |
| 3 | 3.1 Intel I219/I210 | **PARTIAL** — QEMU-validated (e1000e/igb: link up + DHCP DORA); real-PC validation + per-PHY tuning still pending |
| 3 | 3.2 RTL8169 | **DEFERRED** — no QEMU model (QEMU has RTL8139 only, different chip); needs a real box or a new emulation to validate |
| 3 | 3.3 PCIe ECAM | **DONE** (q35-validated: MCFG + MMIO config dispatch, legacy fallback) |
| 3 | 3.4 MSI/MSI-X | **DONE** (q35-validated: e1000e/igb delivery on KVM + TCG, MSI fallback, legacy intact) |
| 4 | 4.1 USB xHCI | **PENDING** (QEMU `qemu-xhci`-validatable) |
| 4 | 4.2 UEFI stub | **PENDING** (OVMF-validatable) |
| 4 | 4.3 Framebuffer console | **PENDING** (QEMU VGA/GOP-validatable) |
