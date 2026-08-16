#ifndef PCI_H
#define PCI_H

#include "types.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION       0x08
#define PCI_PROG_IF        0x09
#define PCI_SUBCLASS       0x0A
#define PCI_CLASS          0x0B
#define PCI_HEADER_TYPE    0x0E
#define PCI_BAR0           0x10
#define PCI_BAR1           0x14
#define PCI_BAR2           0x18
#define PCI_BAR3           0x1C
#define PCI_BAR4           0x20
#define PCI_BAR5           0x24
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_CAP_PTR        0x34

#define PCI_CLASS_STORAGE     0x01
#define PCI_SUBCLASS_IDE      0x01
#define PCI_SUBCLASS_AHCI     0x06
#define PCI_SUBCLASS_NVME     0x08

#define PCI_CLASS_NETWORK     0x02
#define PCI_CLASS_DISPLAY     0x03
#define PCI_CLASS_MULTIMEDIA  0x04
#define PCI_CLASS_BRIDGE      0x06
#define PCI_CLASS_SERIAL      0x0C
#define PCI_SUBCLASS_USB      0x03
#define PCI_SUBCLASS_SATA     0x04

#define MAX_PCI_DEVICES 64

/* PCI capability IDs (PCI 3.0) */
#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_MSIX  0x11

/* MSI-X delivery vector + kernel IRQ number (vector - 32).
 * 0x48 is free: PIC occupies 0x20-0x2F, IPIs 0x41-0x44, syscall 0x80. */
#define PCI_MSIX_VECTOR  0x48
#define PCI_MSIX_IRQ     (PCI_MSIX_VECTOR - 32)

typedef struct {
    uint8_t  cap_offset;
    uint16_t msgctl;        /* message control (cap+2): table size + enable */
    uint8_t  table_bar;     /* BAR index holding the table (cap+4 BIR) */
    uint32_t table_offset;  /* table offset within the BAR (4 KB aligned) */
    uint16_t table_size;    /* number of table entries */
} pci_msix_info_t;

typedef struct {
    uint8_t  cap_offset;
    uint16_t msgctl;        /* message control (cap+2) */
    int      is64;          /* 64-bit addressing capable */
} pci_msi_info_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  rev_id;
    uint8_t  header_type;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint32_t bar[6];
    uint8_t  irq_line;
} pci_device_t;

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
/* Force legacy 0xCF8/0xCFC access (ECAM bypass) — used by the ECAM test */
uint32_t pci_config_read_legacy(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write_legacy(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
int      pci_ecam_active(void);
int      pci_enum_devices(pci_device_t* devices, int max);
void     pci_enable_bus_mastering(pci_device_t* dev);
err_t    pci_init(void);
int      pci_device_count(void);
pci_device_t* pci_get_device(int index);

/* MSI / MSI-X */
err_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id, uint8_t* out_offset);
err_t pci_msix_probe(pci_device_t* dev, pci_msix_info_t* out);
err_t pci_msix_enable(pci_device_t* dev, pci_msix_info_t* info, uint8_t vector);
err_t pci_msi_enable(pci_device_t* dev, uint8_t vector);

#endif
