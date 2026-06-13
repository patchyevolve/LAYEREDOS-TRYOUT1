#include "kernel.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "block.h"
#include "hal.h"
#include "sched.h"

#define AHCI_VADDR_BASE  0xFFFFFFE000000000ULL
#define AHCI_VADDR_PAGE_SIZE 0x1000

static uint64_t ahci_next_vaddr = AHCI_VADDR_BASE;
static uint64_t ahci_vaddr_limit = AHCI_VADDR_BASE + 0x80000;

static ahci_ctrl_t ahci_ctrl;

static void* ahci_alloc_dma(uint64_t* out_phys) {
    if (ahci_next_vaddr >= ahci_vaddr_limit) return NULL;
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    uint64_t virt = ahci_next_vaddr;
    ahci_next_vaddr += AHCI_VADDR_PAGE_SIZE;
    err_t err = vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    if (err != ERR_OK) {
        pmm_free_page(phys);
        ahci_next_vaddr -= AHCI_VADDR_PAGE_SIZE;
        return NULL;
    }
    *out_phys = phys;
    return (void*)virt;
}

static void* ahci_alloc_dma_pages(uint32_t count, uint64_t* out_phys) {
    if (count == 0) return NULL;
    if (ahci_next_vaddr + (uint64_t)count * AHCI_VADDR_PAGE_SIZE > ahci_vaddr_limit)
        return NULL;
    uint64_t phys = pmm_alloc_pages(count);
    if (!phys) return NULL;
    uint64_t virt = ahci_next_vaddr;
    ahci_next_vaddr += (uint64_t)count * AHCI_VADDR_PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        err_t e = vmm_map_page(vmm_get_kernel_pml4(), virt + i * AHCI_VADDR_PAGE_SIZE,
                               phys + i * AHCI_VADDR_PAGE_SIZE,
                               PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (e != ERR_OK) {
            for (uint32_t j = 0; j < i; j++)
                vmm_unmap_page(vmm_get_kernel_pml4(), virt + j * AHCI_VADDR_PAGE_SIZE);
            ahci_next_vaddr -= (uint64_t)count * AHCI_VADDR_PAGE_SIZE;
            pmm_free_pages(phys, count);
            return NULL;
        }
    }
    *out_phys = phys;
    return (void*)virt;
}

static void ahci_free_dma(void* virt, uint64_t phys) {
    if (!virt) return;
    vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)virt);
    if (phys) pmm_free_page(phys);
}

static void ahci_free_dma_pages(void* virt, uint64_t phys, uint32_t count) {
    if (!virt || count == 0) return;
    for (uint32_t i = 0; i < count; i++)
        vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)virt + i * AHCI_VADDR_PAGE_SIZE);
    if (phys) pmm_free_pages(phys, count);
}

static int ahci_decode_bar(uint32_t bar_val, uint64_t* phys_out, int* is_mmio) {
    if (bar_val & 1) {
        *is_mmio = 0;
        *phys_out = (uint64_t)(bar_val & ~0x3F);
    } else {
        *is_mmio = 1;
        *phys_out = (uint64_t)(bar_val & ~0xF);
    }
    return 0;
}

static pci_device_t* ahci_find_controller(void) {
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        pci_device_t* dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->class_code == PCI_CLASS_STORAGE && dev->subclass == PCI_SUBCLASS_AHCI)
            return dev;
    }
    return NULL;
}

static int ahci_wait_bit_clear(volatile uint32_t* reg, uint32_t mask, int timeout_us) {
    for (int i = 0; i < timeout_us; i++) {
        if (!(*reg & mask)) return 0;
        hal_udelay(1);
    }
    return -1;
}

static int ahci_find_slot(ahci_port_t* port) {
    ahci_ctrl_t* ctrl = &ahci_ctrl;
    volatile uint32_t* port_regs = ctrl->abar + (0x100 + port->port_num * 0x80) / 4;
    uint32_t ci = port_regs[AHCI_PxCI / 4];
    uint32_t sts = port_regs[AHCI_PxCMD / 4];
    if (!(sts & AHCI_PxCMD_ST)) return -1;
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        if (!(ci & (1 << i))) return i;
    }
    return -1;
}

static int ahci_issue_cmd(ahci_port_t* port, int slot, uint8_t cmd,
                          uint64_t lba, uint16_t sector_count,
                          uint64_t data_phys, uint32_t num_pages, int write) {
    ahci_ctrl_t* ctrl = &ahci_ctrl;
    volatile uint32_t* port_regs = ctrl->abar + (0x100 + port->port_num * 0x80) / 4;

    ahci_cmd_hdr_t* cmd_hdr = &port->cmd_list[slot];
    ahci_cmd_table_t* ct = port->cmd_table;
    uint64_t ct_phys = port->cmd_table_phys;

    kmemset(ct, 0, sizeof(ahci_cmd_table_t));

    uint8_t* cfis = ct->cfis;
    cfis[0] = 0x27;
    cfis[1] = 0x80;
    cfis[2] = cmd;
    cfis[3] = 0;
    cfis[4] = (uint8_t)(lba & 0xFF);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFF);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFF);
    cfis[7] = 0x40 | 0xE0;
    cfis[8] = (uint8_t)((lba >> 24) & 0xFF);
    cfis[9] = (uint8_t)((lba >> 32) & 0xFF);
    cfis[10] = (uint8_t)((lba >> 40) & 0xFF);
    cfis[11] = 0;
    cfis[12] = (uint8_t)(sector_count & 0xFF);
    cfis[13] = (uint8_t)((sector_count >> 8) & 0xFF);

    kmemset(cmd_hdr, 0, sizeof(ahci_cmd_hdr_t));
    cmd_hdr->cfis_length = sizeof(ct->cfis) / 4;
    cmd_hdr->write = write ? 1 : 0;
    cmd_hdr->prdtl = num_pages;
    cmd_hdr->ctba  = (uint32_t)(ct_phys & 0xFFFFFFFF);
    cmd_hdr->ctbau = (uint32_t)((ct_phys >> 32) & 0xFFFFFFFF);
    cmd_hdr->prdbc = 0;

    uint32_t remaining = (uint32_t)sector_count * AHCI_SECTOR_SIZE;
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)((uint8_t*)ct + sizeof(ahci_cmd_table_t));
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t chunk = (remaining < AHCI_VADDR_PAGE_SIZE) ? remaining : AHCI_VADDR_PAGE_SIZE;
        uint64_t page_phys = data_phys + (uint64_t)i * AHCI_VADDR_PAGE_SIZE;
        prdt[i].dba  = (uint32_t)(page_phys & 0xFFFFFFFF);
        prdt[i].dbau = (uint32_t)((page_phys >> 32) & 0xFFFFFFFF);
        prdt[i].dbc  = (chunk - 1) | (1 << 31);
        remaining -= chunk;
    }

    port_regs[AHCI_PxCI / 4] = (1 << slot);

    if (ahci_wait_bit_clear(port_regs + AHCI_PxCI / 4, 1 << slot, 5000000)) {
        uint32_t serr = port_regs[AHCI_PxSERR / 4];
        if (serr) port_regs[AHCI_PxSERR / 4] = serr;
        return -1;
    }

    uint32_t tfd = port_regs[AHCI_PxTFD / 4];
    if (tfd & 1) {
        uint32_t serr = port_regs[AHCI_PxSERR / 4];
        if (serr) port_regs[AHCI_PxSERR / 4] = serr;
        return -1;
    }

    return (int)((uint32_t)sector_count * AHCI_SECTOR_SIZE);
}

static int ahci_identify(ahci_port_t* port) {
    cpu_flags_t flags;
    spinlock_acquire(&ahci_ctrl.lock, &flags);

    int slot = ahci_find_slot(port);
    if (slot < 0) {
        spinlock_release(&ahci_ctrl.lock, flags);
        return -1;
    }

    uint64_t bounce_phys;
    void* bounce = ahci_alloc_dma(&bounce_phys);
    if (!bounce) {
        spinlock_release(&ahci_ctrl.lock, flags);
        return -1;
    }
    kmemset(bounce, 0, 4096);

    int ret = ahci_issue_cmd(port, slot, 0xEC, 0, 1, bounce_phys, 1, 0);

    spinlock_release(&ahci_ctrl.lock, flags);

    if (ret > 0) {
        uint16_t* id_buf = (uint16_t*)bounce;
        uint32_t sectors_28 = (uint32_t)id_buf[60] | ((uint32_t)id_buf[61] << 16);
        uint32_t sectors_48_lo = (uint32_t)id_buf[100] | ((uint32_t)id_buf[101] << 16);
        uint32_t sectors_48_hi = (uint32_t)id_buf[102] | ((uint32_t)id_buf[103] << 16);
        port->sector_count = sectors_48_lo
            ? ((uint64_t)sectors_48_hi << 32 | sectors_48_lo) : sectors_28;
        int i;
        for (i = 0; i < 40; i += 2) {
            port->model[i]     = id_buf[27 + i/2] >> 8;
            port->model[i + 1] = id_buf[27 + i/2] & 0xFF;
        }
        port->model[40] = '\0';
        i = 39;
        while (i > 0 && port->model[i] == ' ') port->model[i--] = '\0';
    }

    ahci_free_dma(bounce, bounce_phys);
    return (ret > 0) ? 0 : -1;
}

static int ahci_port_init(ahci_port_t* port) {
    ahci_ctrl_t* ctrl = &ahci_ctrl;
    volatile uint32_t* port_regs = ctrl->abar + (0x100 + port->port_num * 0x80) / 4;

    uint32_t ssts = port_regs[AHCI_PxSSTS / 4];
    uint32_t det = ssts & AHCI_PxSSTS_DET_MASK;
    uint32_t ipm = (ssts >> 8) & 0x0F;
    if ((det != AHCI_PxSSTS_DET_PRES && det != AHCI_PxSSTS_DET_ACTIVE) ||
        ipm != AHCI_PxSSTS_IPM_ACTIVE)
        return -1;

    uint32_t sig = port_regs[AHCI_PxSIG / 4];
    if (sig != AHCI_SIG_ATA) return -1;

    port_regs[AHCI_PxCMD / 4] &= ~AHCI_PxCMD_ST;
    ahci_wait_bit_clear(port_regs + AHCI_PxCMD / 4, AHCI_PxCMD_CR, 500000);

    port_regs[AHCI_PxCMD / 4] &= ~AHCI_PxCMD_FRE;
    ahci_wait_bit_clear(port_regs + AHCI_PxCMD / 4, AHCI_PxCMD_FR, 500000);

    port->cmd_list_phys = pmm_alloc_page();
    if (!port->cmd_list_phys) return -1;
    {
        uint64_t virt = ahci_next_vaddr;
        ahci_next_vaddr += AHCI_VADDR_PAGE_SIZE;
        if (virt >= ahci_vaddr_limit) {
            pmm_free_page(port->cmd_list_phys);
            return -1;
        }
        err_t e = vmm_map_page(vmm_get_kernel_pml4(), virt, port->cmd_list_phys,
                               PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (e != ERR_OK) {
            pmm_free_page(port->cmd_list_phys);
            return -1;
        }
        port->cmd_list = (ahci_cmd_hdr_t*)virt;
        port->fis      = (ahci_fis_t*)virt;
        port->fis_phys = port->cmd_list_phys;
        kmemset((void*)virt, 0, 4096);
    }

    port->cmd_table_phys = pmm_alloc_page();
    if (!port->cmd_table_phys) {
        pmm_free_page(port->cmd_list_phys);
        return -1;
    }
    {
        uint64_t virt = ahci_next_vaddr;
        ahci_next_vaddr += AHCI_VADDR_PAGE_SIZE;
        if (virt >= ahci_vaddr_limit) {
            pmm_free_page(port->cmd_table_phys);
            pmm_free_page(port->cmd_list_phys);
            return -1;
        }
        err_t e = vmm_map_page(vmm_get_kernel_pml4(), virt, port->cmd_table_phys,
                               PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (e != ERR_OK) {
            pmm_free_page(port->cmd_table_phys);
            pmm_free_page(port->cmd_list_phys);
            return -1;
        }
        port->cmd_table = (ahci_cmd_table_t*)virt;
        kmemset((void*)virt, 0, 4096);
    }

    port_regs[AHCI_PxCLB / 4]  = (uint32_t)(port->cmd_list_phys & 0xFFFFFFFF);
    port_regs[AHCI_PxCLBU / 4] = (uint32_t)((port->cmd_list_phys >> 32) & 0xFFFFFFFF);
    port_regs[AHCI_PxFB / 4]   = (uint32_t)(port->fis_phys & 0xFFFFFFFF);
    port_regs[AHCI_PxFBU / 4]  = (uint32_t)((port->fis_phys >> 32) & 0xFFFFFFFF);

    port_regs[AHCI_PxSERR / 4] = 0xFFFFFFFF;

    port_regs[AHCI_PxCMD / 4] |= AHCI_PxCMD_FRE;
    port_regs[AHCI_PxCMD / 4] |= AHCI_PxCMD_ST;

    port->present = 1;
    return 0;
}

/* ---- Block device operations ---- */

static err_t ahci_blk_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    uint32_t port_idx = (uint32_t)(uintptr_t)dev->private_data;
    ahci_port_t* port = &ahci_ctrl.ports[port_idx];
    if (!port->present) return ERR_IO;

    size_t total = (size_t)count * AHCI_SECTOR_SIZE;
    uint32_t num_pages = (uint32_t)((total + AHCI_VADDR_PAGE_SIZE - 1) / AHCI_VADDR_PAGE_SIZE);

    uint64_t bounce_phys;
    void* bounce;
    if (num_pages > 1)
        bounce = ahci_alloc_dma_pages(num_pages, &bounce_phys);
    else
        bounce = ahci_alloc_dma(&bounce_phys);
    if (!bounce) return ERR_NOMEM;
    kmemset(bounce, 0, total);

    cpu_flags_t flags;
    spinlock_acquire(&ahci_ctrl.lock, &flags);

    int slot = ahci_find_slot(port);
    if (slot < 0) {
        spinlock_release(&ahci_ctrl.lock, flags);
        if (num_pages > 1) ahci_free_dma_pages(bounce, bounce_phys, num_pages);
        else ahci_free_dma(bounce, bounce_phys);
        return ERR_IO;
    }

    int ret = ahci_issue_cmd(port, slot, 0x25, lba, count, bounce_phys, num_pages, 0);

    spinlock_release(&ahci_ctrl.lock, flags);

    if (ret > 0) {
        kmemcpy(buf, bounce, total);
    }

    if (num_pages > 1) ahci_free_dma_pages(bounce, bounce_phys, num_pages);
    else ahci_free_dma(bounce, bounce_phys);
    return (ret > 0) ? ERR_OK : ERR_IO;
}

static err_t ahci_blk_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    uint32_t port_idx = (uint32_t)(uintptr_t)dev->private_data;
    ahci_port_t* port = &ahci_ctrl.ports[port_idx];
    if (!port->present) return ERR_IO;

    size_t total = (size_t)count * AHCI_SECTOR_SIZE;
    uint32_t num_pages = (uint32_t)((total + AHCI_VADDR_PAGE_SIZE - 1) / AHCI_VADDR_PAGE_SIZE);

    uint64_t bounce_phys;
    void* bounce;
    if (num_pages > 1)
        bounce = ahci_alloc_dma_pages(num_pages, &bounce_phys);
    else
        bounce = ahci_alloc_dma(&bounce_phys);
    if (!bounce) return ERR_NOMEM;
    kmemcpy(bounce, buf, total);

    cpu_flags_t flags;
    spinlock_acquire(&ahci_ctrl.lock, &flags);

    int slot = ahci_find_slot(port);
    if (slot < 0) {
        spinlock_release(&ahci_ctrl.lock, flags);
        if (num_pages > 1) ahci_free_dma_pages(bounce, bounce_phys, num_pages);
        else ahci_free_dma(bounce, bounce_phys);
        return ERR_IO;
    }

    int ret = ahci_issue_cmd(port, slot, 0x35, lba, count, bounce_phys, num_pages, 1);

    spinlock_release(&ahci_ctrl.lock, flags);

    if (num_pages > 1) ahci_free_dma_pages(bounce, bounce_phys, num_pages);
    else ahci_free_dma(bounce, bounce_phys);
    return (ret > 0) ? ERR_OK : ERR_IO;
}

/* ---- Initialisation ---- */

err_t ahci_init(void) {
    kmemset(&ahci_ctrl, 0, sizeof(ahci_ctrl));
    spinlock_init(&ahci_ctrl.lock, "ahci");
    ahci_next_vaddr = AHCI_VADDR_BASE;

    pci_device_t* dev = ahci_find_controller();
    if (!dev) {
        kprintf("[AHCI] No AHCI controller found\n");
        return ERR_NOENT;
    }

    kprintf("[AHCI] Found %04x:%04x at %02x:%02x.%d\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func);

    pci_enable_bus_mastering(dev);

    int is_mmio;
    uint64_t abar_phys;
    ahci_decode_bar(dev->bar[5], &abar_phys, &is_mmio);
    if (!is_mmio || !abar_phys) {
        kprintf("[AHCI] ABAR is not MMIO\n");
        return ERR_NOENT;
    }

    {
        uint64_t abar_virt = ahci_next_vaddr;
        ahci_next_vaddr += AHCI_VADDR_PAGE_SIZE;
        if (abar_virt >= ahci_vaddr_limit) return ERR_NOMEM;
        err_t e = vmm_map_page(vmm_get_kernel_pml4(), abar_virt, abar_phys,
                               PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (e != ERR_OK) return e;
        ahci_ctrl.abar = (volatile uint32_t*)abar_virt;
    }
    ahci_ctrl.abar_phys = abar_phys;

    uint32_t cap = ahci_ctrl.abar[AHCI_CAP / 4];
    ahci_ctrl.n_ports = (int)((cap & 0x1F) + 1);
    ahci_ctrl.ports_impl = ahci_ctrl.abar[AHCI_PI / 4];
    uint32_t vs = ahci_ctrl.abar[AHCI_VS / 4];

    kprintf("[AHCI] v%d.%d, %d ports, mask=0x%08x\n",
            (vs >> 16) & 0xFF, vs & 0xFFFF, ahci_ctrl.n_ports, ahci_ctrl.ports_impl);

    ahci_ctrl.abar[AHCI_GHC / 4] |= AHCI_GHC_HR;
    if (ahci_wait_bit_clear(ahci_ctrl.abar + AHCI_GHC / 4, AHCI_GHC_HR, 1000000)) {
        kprintf("[AHCI] HBA reset timeout\n");
        return ERR_IO;
    }

    ahci_ctrl.abar[AHCI_GHC / 4] |= AHCI_GHC_AE;
    ahci_ctrl.abar[AHCI_GHC / 4] |= AHCI_GHC_IE;

    int found = 0;
    for (int i = 0; i < AHCI_MAX_PORTS && i < ahci_ctrl.n_ports; i++) {
        if (!(ahci_ctrl.ports_impl & (1 << i))) continue;

        ahci_ctrl.ports[i].port_num = (uint32_t)i;
        kprintf("[AHCI] Port %d: probing...\n", i);

        if (ahci_port_init(&ahci_ctrl.ports[i]) != 0) continue;

        if (ahci_identify(&ahci_ctrl.ports[i]) != 0) {
            ahci_ctrl.ports[i].present = 0;
            continue;
        }

        kprintf("[AHCI] Port %d: %s (%llu sectors, %llu MB)\n",
                i, ahci_ctrl.ports[i].model,
                ahci_ctrl.ports[i].sector_count,
                ahci_ctrl.ports[i].sector_count / 2048);

        block_dev_t bd;
        kmemset(&bd, 0, sizeof(bd));
        kstrncpy(bd.name, "ahci", sizeof(bd.name) - 1);
        {
            char suffix[8];
            int slen = kstrlen(bd.name);
            suffix[0] = '0' + (char)i;
            suffix[1] = '\0';
            kstrncat(bd.name, suffix, sizeof(bd.name) - (size_t)slen - 1);
        }
        bd.block_count = ahci_ctrl.ports[i].sector_count;
        bd.block_size = AHCI_SECTOR_SIZE;
        bd.read = ahci_blk_read;
        bd.write = ahci_blk_write;
        bd.private_data = (void*)(uintptr_t)(uint32_t)i;

        int idx = block_register(&bd);
        if (idx >= 0) {
            kprintf("[AHCI] Registered '%s' (%llu blocks)\n",
                    bd.name, bd.block_count);
            found++;
        }
    }

    if (found == 0) {
        kprintf("[AHCI] No ATA devices on any port\n");
        return ERR_NOENT;
    }

    kprintf("[AHCI] %d device(s) initialized\n", found);
    return ERR_OK;
}
