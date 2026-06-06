# pci.c — PCI Bus Enumerator

**Path:** `os/src/kernel/pci.c`  
**Layer:** Layer 1 (HAL) — bus driver

---

## Purpose

Enumerates all PCI devices on the system bus by walking the PCI
configuration space.  Stores discovered devices in a flat array for
other drivers to query.  Does not implement any device-specific
drivers — it only provides discovery and configuration space access.

---

## PCI configuration space

x86 PCI uses two 32-bit I/O ports:

| Port | Name | Function |
|------|------|---------|
| `0xCF8` | `PCI_CONFIG_ADDR` | Write address: `enable\|bus\|slot\|func\|offset` |
| `0xCFC` | `PCI_CONFIG_DATA` | Read/write configuration data |

Address format (32 bits):
```
Bit 31:    Enable bit (must be 1)
Bits 23:16 Bus number (0–255)
Bits 15:11 Device/slot (0–31)
Bits 10:8  Function (0–7)
Bits 7:2   Register offset (4-byte aligned)
Bits 1:0   Always 0
```

`pci_config_read` and `pci_config_write` wrap this protocol.

---

## Enumeration algorithm

`pci_init` checks if the root bus (bus 0, slot 0, func 0) has a
multi-function header (bit 7 of header type).  If so, it scans all 256
buses; otherwise just bus 0.

For each bus, `pci_scan_bus` scans all 32 slots.
For each slot with a valid vendor ID (`!= 0xFFFF`), `pci_scan_slot`
reads function 0.  If the header type has the multi-function bit set,
it also scans functions 1–7.

For each valid function, `pci_scan_function` reads the full device info:
vendor/device ID, class/subclass, BARs, and IRQ line.

**PCI-to-PCI bridges** (`class=0x06, subclass=0x04`) trigger recursive
enumeration of the secondary bus number from register 0x18.

---

## `pci_read_device` (internal)

Reads all relevant configuration registers into `pci_device_t`:
- Words 0x00–0x01: vendor and device ID
- Byte 0x08: revision, prog_if, subclass, class
- Byte 0x0E: header type
- Words 0x10–0x24: 6 × BAR registers
- Byte 0x3C: IRQ line

---

## `pci_enable_bus_mastering`

Sets bit 2 (Bus Master Enable) and bit 0 (I/O Space Enable) in the
PCI Command register (offset 0x04).  Required before DMA-capable devices
can initiate bus transactions.

---

## `inl` / `outl` dependency

`pci.c` calls `outl(PCI_CONFIG_ADDR, addr)` and `inl(PCI_CONFIG_DATA)`.
These 32-bit port I/O wrappers must be available — either declared in
`hal.h` or as local inline functions.  Check that `hal.c` or a shared
header provides `inl`/`outl`.

---

## Output on `pci_init`

```
[PCI] Enumerated N devices
  00:00.0  8086:1237  class=06 subclass=00 irq=0
  00:01.0  8086:7000  class=06 subclass=01 irq=0
  ...
```
