#include "kernel.h"
#include "pci.h"
#include "hal.h"

static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_count = 0;

static void pci_out_config_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)1 << 31
                  | (uint32_t)bus << 16
                  | (uint32_t)slot << 11
                  | (uint32_t)func << 8
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
}

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    pci_out_config_addr(bus, slot, func, offset);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    pci_out_config_addr(bus, slot, func, offset);
    outl(PCI_CONFIG_DATA, value);
}

static void pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t* dev) {
    uint32_t id = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = (id >> 16) & 0xFFFF;

    uint32_t class_rev = pci_config_read(bus, slot, func, PCI_REVISION);
    dev->rev_id = class_rev & 0xFF;
    dev->prog_if = (class_rev >> 8) & 0xFF;
    dev->subclass = (class_rev >> 16) & 0xFF;
    dev->class_code = (class_rev >> 24) & 0xFF;

    uint32_t hdr = pci_config_read(bus, slot, func, PCI_HEADER_TYPE);
    dev->header_type = hdr & 0xFF;

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;

    for (int i = 0; i < 6; i++) {
        dev->bar[i] = pci_config_read(bus, slot, func, PCI_BAR0 + i * 4);
    }

    uint32_t irq_reg = pci_config_read(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->irq_line = irq_reg & 0xFF;
}

void pci_enable_bus_mastering(pci_device_t* dev) {
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (1 << 2) | (1 << 0);
    pci_config_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

static void pci_scan_bus(int bus);

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (pci_count >= MAX_PCI_DEVICES) return;

    pci_device_t* dev = &pci_devices[pci_count];
    pci_read_device(bus, slot, func, dev);

    if (dev->vendor_id == 0xFFFF || dev->vendor_id == 0x0000)
        return;

    pci_count++;

    if (dev->class_code == PCI_CLASS_BRIDGE && dev->subclass == 0x04) {
        uint32_t bus_reg = pci_config_read(bus, slot, func, 0x18);
        uint8_t secondary_bus = (bus_reg >> 8) & 0xFF;
        if (secondary_bus != bus)
            pci_scan_bus(secondary_bus);
    }
}

static void pci_scan_slot(uint8_t bus, uint8_t slot) {
    uint32_t id = pci_config_read(bus, slot, 0, PCI_VENDOR_ID);
    if (id == 0xFFFF || id == 0x0000) return;

    pci_scan_function(bus, slot, 0);

    uint32_t hdr = pci_config_read(bus, slot, 0, PCI_HEADER_TYPE);
    if (hdr & (1 << 7)) {
        for (int func = 1; func < 8; func++) {
            uint32_t fid = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
            if (fid != 0xFFFF && fid != 0x0000)
                pci_scan_function(bus, slot, func);
        }
    }
}

static void pci_scan_bus(int bus) {
    for (int slot = 0; slot < 32; slot++)
        pci_scan_slot(bus, slot);
}

err_t pci_init(void) {
    kmemset(pci_devices, 0, sizeof(pci_devices));
    pci_count = 0;

    uint32_t hdr = pci_config_read(0, 0, 0, PCI_HEADER_TYPE);
    if (hdr & (1 << 7)) {
        for (int bus = 0; bus < 256; bus++)
            pci_scan_bus(bus);
    } else {
        pci_scan_bus(0);
    }

    kprintf("[PCI] Enumerated %d devices\n", pci_count);
    for (int i = 0; i < pci_count; i++) {
        pci_device_t* d = &pci_devices[i];
        kprintf("  %02x:%02x.%d  %04x:%04x  class=%02x subclass=%02x irq=%d\n",
                d->bus, d->slot, d->func,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->irq_line);
    }
    return ERR_OK;
}

int pci_device_count(void) { return pci_count; }

pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= pci_count) return NULL;
    return &pci_devices[index];
}
