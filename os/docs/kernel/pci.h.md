# pci.h — PCI Bus Interface

**Path:** `os/src/kernel/pci.h`  
**Layer:** Layer 1 (HAL) — header

---

## Purpose

Declares PCI configuration space port addresses, standard register offsets,
class/subclass codes, the `pci_device_t` struct, and the enumeration API.

---

## I/O port constants

```c
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
```

---

## Configuration register offsets

| Constant | Offset | Size | Content |
|----------|--------|------|---------|
| `PCI_VENDOR_ID` | 0x00 | 16-bit | Vendor (0x8086=Intel, 0x1022=AMD…) |
| `PCI_DEVICE_ID` | 0x02 | 16-bit | Device model |
| `PCI_COMMAND` | 0x04 | 16-bit | I/O enable, Bus Master, etc. |
| `PCI_STATUS` | 0x06 | 16-bit | Capabilities present, error flags |
| `PCI_REVISION` | 0x08 | 8-bit | Silicon revision |
| `PCI_PROG_IF` | 0x09 | 8-bit | Programming interface |
| `PCI_SUBCLASS` | 0x0A | 8-bit | Device subclass |
| `PCI_CLASS` | 0x0B | 8-bit | Device class |
| `PCI_HEADER_TYPE` | 0x0E | 8-bit | 0=endpoint, 1=PCI bridge; bit7=multi-func |
| `PCI_BAR0`–`PCI_BAR5` | 0x10–0x24 | 32-bit each | Base Address Registers |
| `PCI_INTERRUPT_LINE` | 0x3C | 8-bit | IRQ line (BIOS-assigned) |

---

## Class/subclass codes used

```c
PCI_CLASS_STORAGE  (0x01)   // ATA, AHCI, NVMe
PCI_SUBCLASS_IDE   (0x01)   // ATA/IDE controller
PCI_CLASS_NETWORK  (0x02)   // Network cards
PCI_CLASS_BRIDGE   (0x06)   // PCI-to-PCI bridges
PCI_SUBCLASS_USB   (0x03)   // USB controller
```

---

## `pci_device_t`

```c
typedef struct {
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, rev_id;
    uint8_t  header_type;
    uint8_t  bus, slot, func;     // location on PCI bus
    uint32_t bar[6];              // BAR values (I/O or MMIO addresses)
    uint8_t  irq_line;            // IRQ line number from config space
} pci_device_t;
```

---

## API

| Function | Description |
|----------|-------------|
| `pci_init()` | Enumerate all devices, print summary |
| `pci_config_read(bus, slot, func, offset)` | Read 32-bit config register |
| `pci_config_write(bus, slot, func, offset, value)` | Write 32-bit config register |
| `pci_enum_devices(devices, max)` | Copy discovered devices into caller's array |
| `pci_enable_bus_mastering(dev)` | Set Command.BusMaster + Command.IOEnable |
| `pci_device_count()` | Number of discovered devices |
| `pci_get_device(index)` | Pointer to device by index |
