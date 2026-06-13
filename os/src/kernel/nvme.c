#include "kernel.h"
#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "block.h"
#include "hal.h"
#include "sched.h"

#define NVME_VADDR_BASE  0xFFFFFFC000000000ULL
#define NVME_VADDR_LIMIT (NVME_VADDR_BASE + 0x80000)

static uint64_t nvme_next_vaddr = NVME_VADDR_BASE;
static nvme_ctrl_t nvme_ctrl;

static uint64_t nvme_bounce_phys = 0;
static void*    nvme_bounce      = NULL;

static void* nvme_alloc_page(uint64_t* out_phys) {
    if (nvme_next_vaddr >= NVME_VADDR_LIMIT) return NULL;
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    uint64_t virt = nvme_next_vaddr;
    nvme_next_vaddr += 0x1000;
    err_t e = vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                           PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    if (e != ERR_OK) {
        pmm_free_page(phys);
        nvme_next_vaddr -= 0x1000;
        return NULL;
    }
    *out_phys = phys;
    return (void*)virt;
}

static void nvme_free_page(void* virt, uint64_t phys) {
    if (virt) {
        vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)virt);
        nvme_next_vaddr -= 0x1000;
    }
    if (phys) pmm_free_page(phys);
}

static uint64_t nvme_read64(volatile uint32_t* regs, uint32_t off) {
    return (uint64_t)regs[off / 4] | ((uint64_t)regs[off / 4 + 1] << 32);
}

static void nvme_write64(volatile uint32_t* regs, uint32_t off, uint64_t val) {
    regs[off / 4]     = (uint32_t)(val & 0xFFFFFFFF);
    regs[off / 4 + 1] = (uint32_t)((val >> 32) & 0xFFFFFFFF);
}

static int nvme_wait_bit(volatile uint32_t* reg, uint32_t mask, int set, int timeout_ms) {
    uint64_t deadline = hal_timer_get_ns() + (uint64_t)timeout_ms * 1000000;
    for (;;) {
        if (set ? (*reg & mask) : !(*reg & mask)) return 0;
        if (hal_timer_get_ns() >= deadline) return -1;
        hal_udelay(10);
    }
}

static uint16_t nvme_next_cid(void) {
    return nvme_ctrl.next_cid++;
}

static int nvme_poll_cpl(nvme_cpl_t* cq, volatile int* phase, int timeout_ms) {
    uint64_t deadline = hal_timer_get_ns() + (uint64_t)timeout_ms * 1000000;
    for (;;) {
        uint16_t status = cq[0].status;
        int p = (status >> 14) & 1;
        if (p == *phase) {
            int sc  = status & 0xFF;
            int sct = (status >> 9) & 7;
            *phase = *phase ? 0 : 1;
            return (sc == 0 && sct == 0) ? 0 : -1;
        }
        if (hal_timer_get_ns() >= deadline) return -1;
        hal_udelay(10);
    }
}

static int nvme_admin_cmd(nvme_cmd_t* cmd, uint64_t prp1) {
    nvme_ctrl_t* ctrl = &nvme_ctrl;
    cmd->prp1 = prp1;
    cmd->prp2 = 0;

    ctrl->asq[0] = *cmd;
    asm volatile("mfence" ::: "memory");

    uint32_t* db = (uint32_t*)((uintptr_t)ctrl->regs + ctrl->doorbell_stride * 0);
    *db = 1;

    return nvme_poll_cpl(ctrl->acq, &ctrl->acq_phase, ctrl->timeout_500ms * 500);
}

static int nvme_io_cmd(uint8_t opcode, uint64_t slba, uint16_t nblocks,
                        uint64_t prp1, uint64_t prp2) {
    nvme_ctrl_t* ctrl = &nvme_ctrl;

    nvme_cmd_t cmd;
    kmemset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.cid    = 0;
    cmd.nsid   = 1;
    cmd.prp1   = prp1;
    cmd.prp2   = prp2;
    cmd.cdw10  = (uint32_t)(slba & 0xFFFFFFFF);
    cmd.cdw11  = (uint32_t)((slba >> 32) & 0xFFFFFFFF);
    cmd.cdw12  = (uint32_t)(nblocks - 1);

    ctrl->iosq[0] = cmd;
    asm volatile("mfence" ::: "memory");

    uint32_t db_off = (uint32_t)(ctrl->doorbell_stride * 2);
    uint32_t* db = (uint32_t*)((uintptr_t)ctrl->regs + db_off);
    *db = 1;

    return nvme_poll_cpl(ctrl->iocq, &ctrl->iocq_phase, 5000);
}

static pci_device_t* nvme_find_controller(void) {
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        pci_device_t* dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->class_code == PCI_CLASS_STORAGE && dev->subclass == PCI_SUBCLASS_NVME)
            return dev;
    }
    return NULL;
}

static uint64_t nvme_decode_bar(uint32_t bar_val) {
    return (uint64_t)(bar_val & ~0xF);
}

/* ---- Block device operations ---- */

static err_t nvme_blk_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    (void)dev;
    if (!nvme_ctrl.present) return ERR_IO;

    uint64_t byte_count = (uint64_t)count << nvme_ctrl.lba_shift;
    uint32_t num_pages = (uint32_t)((byte_count + 0xFFF) >> 12);
    uint64_t data_phys, prp1, prp2 = 0;
    void* data_virt;
    uint64_t prp_list_phys = 0;
    int need_cleanup = 0;
    int ret;

    if (num_pages == 1) {
        data_phys = nvme_bounce_phys;
        data_virt = nvme_bounce;
    } else {
        data_phys = pmm_alloc_pages(num_pages);
        if (!data_phys) return ERR_NOMEM;
        need_cleanup = 1;
        if (nvme_next_vaddr + (uint64_t)num_pages * 0x1000 > NVME_VADDR_LIMIT) {
            pmm_free_pages(data_phys, num_pages);
            return ERR_NOMEM;
        }
        uint64_t dv = nvme_next_vaddr;
        nvme_next_vaddr += (uint64_t)num_pages * 0x1000;
        for (uint32_t i = 0; i < num_pages; i++) {
            err_t e = vmm_map_page(vmm_get_kernel_pml4(), dv + i * 0x1000,
                                   data_phys + i * 0x1000,
                                   PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
            if (e != ERR_OK) {
                for (uint32_t j = 0; j < i; j++)
                    vmm_unmap_page(vmm_get_kernel_pml4(), dv + j * 0x1000);
                nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
                pmm_free_pages(data_phys, num_pages);
                return e;
            }
        }
        data_virt = (void*)dv;
    }

    prp1 = data_phys;
    if (num_pages > 2) {
        prp_list_phys = pmm_alloc_page();
        if (!prp_list_phys) {
            if (need_cleanup) {
                for (uint32_t i = 0; i < num_pages; i++)
                    vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)data_virt + i * 0x1000);
                nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
                pmm_free_pages(data_phys, num_pages);
            }
            return ERR_NOMEM;
        }
        uint64_t* prp_list = (uint64_t*)PHYS_TO_VIRT(prp_list_phys);
        for (uint32_t i = 1; i < num_pages; i++)
            prp_list[i - 1] = data_phys + (uint64_t)i * 0x1000;
        prp2 = prp_list_phys;
    } else if (num_pages == 2) {
        prp2 = data_phys + 0x1000;
    }

    cpu_flags_t flags;
    spinlock_acquire(&nvme_ctrl.lock, &flags);
    ret = nvme_io_cmd(NVME_IO_READ, lba, count, prp1, prp2);
    spinlock_release(&nvme_ctrl.lock, flags);

    if (ret == 0)
        kmemcpy(buf, data_virt, (size_t)byte_count);

    if (prp_list_phys)
        pmm_free_page(prp_list_phys);
    if (need_cleanup) {
        for (uint32_t i = 0; i < num_pages; i++)
            vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)data_virt + i * 0x1000);
        nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
        pmm_free_pages(data_phys, num_pages);
    }

    return (ret == 0) ? ERR_OK : ERR_IO;
}

static err_t nvme_blk_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    (void)dev;
    if (!nvme_ctrl.present) return ERR_IO;

    uint64_t byte_count = (uint64_t)count << nvme_ctrl.lba_shift;
    uint32_t num_pages = (uint32_t)((byte_count + 0xFFF) >> 12);
    uint64_t data_phys, prp1, prp2 = 0;
    void* data_virt;
    uint64_t prp_list_phys = 0;
    int need_cleanup = 0;
    int ret;

    if (num_pages == 1) {
        data_phys = nvme_bounce_phys;
        data_virt = nvme_bounce;
    } else {
        data_phys = pmm_alloc_pages(num_pages);
        if (!data_phys) return ERR_NOMEM;
        need_cleanup = 1;
        if (nvme_next_vaddr + (uint64_t)num_pages * 0x1000 > NVME_VADDR_LIMIT) {
            pmm_free_pages(data_phys, num_pages);
            return ERR_NOMEM;
        }
        uint64_t dv = nvme_next_vaddr;
        nvme_next_vaddr += (uint64_t)num_pages * 0x1000;
        for (uint32_t i = 0; i < num_pages; i++) {
            err_t e = vmm_map_page(vmm_get_kernel_pml4(), dv + i * 0x1000,
                                   data_phys + i * 0x1000,
                                   PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
            if (e != ERR_OK) {
                for (uint32_t j = 0; j < i; j++)
                    vmm_unmap_page(vmm_get_kernel_pml4(), dv + j * 0x1000);
                nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
                pmm_free_pages(data_phys, num_pages);
                return e;
            }
        }
        data_virt = (void*)dv;
    }

    kmemcpy(data_virt, buf, (size_t)byte_count);

    prp1 = data_phys;
    if (num_pages > 2) {
        prp_list_phys = pmm_alloc_page();
        if (!prp_list_phys) {
            if (need_cleanup) {
                for (uint32_t i = 0; i < num_pages; i++)
                    vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)data_virt + i * 0x1000);
                nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
                pmm_free_pages(data_phys, num_pages);
            }
            return ERR_NOMEM;
        }
        uint64_t* prp_list = (uint64_t*)PHYS_TO_VIRT(prp_list_phys);
        for (uint32_t i = 1; i < num_pages; i++)
            prp_list[i - 1] = data_phys + (uint64_t)i * 0x1000;
        prp2 = prp_list_phys;
    } else if (num_pages == 2) {
        prp2 = data_phys + 0x1000;
    }

    cpu_flags_t flags;
    spinlock_acquire(&nvme_ctrl.lock, &flags);
    ret = nvme_io_cmd(NVME_IO_WRITE, lba, count, prp1, prp2);
    spinlock_release(&nvme_ctrl.lock, flags);

    if (prp_list_phys)
        pmm_free_page(prp_list_phys);
    if (need_cleanup) {
        for (uint32_t i = 0; i < num_pages; i++)
            vmm_unmap_page(vmm_get_kernel_pml4(), (uint64_t)data_virt + i * 0x1000);
        nvme_next_vaddr -= (uint64_t)num_pages * 0x1000;
        pmm_free_pages(data_phys, num_pages);
    }

    return (ret == 0) ? ERR_OK : ERR_IO;
}

/* ---- Initialisation ---- */

err_t nvme_init(void) {
    kmemset(&nvme_ctrl, 0, sizeof(nvme_ctrl));
    spinlock_init(&nvme_ctrl.lock, "nvme");
    nvme_next_vaddr = NVME_VADDR_BASE;

    pci_device_t* dev = nvme_find_controller();
    if (!dev) {
        kprintf("[NVMe] No NVMe controller found\n");
        return ERR_NOENT;
    }

    kprintf("[NVMe] Found %04x:%04x at %02x:%02x.%d\n",
            dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func);

    pci_enable_bus_mastering(dev);

    {
        uint64_t bar0_phys = nvme_decode_bar(dev->bar[0]);
        if (!bar0_phys) { kprintf("[NVMe] BAR0 invalid\n"); return ERR_IO; }
        void* regs = nvme_alloc_page(&bar0_phys);
        if (!regs) return ERR_NOMEM;
        nvme_ctrl.regs = (volatile uint32_t*)regs;
        nvme_ctrl.regs_phys = bar0_phys;
    }

    uint64_t cap = nvme_read64(nvme_ctrl.regs, NVME_CAP);
    uint32_t vs = nvme_ctrl.regs[NVME_VS / 4];

    int mqes  = (int)NVME_CAP_MQES(cap);
    int dstrd = (int)NVME_CAP_DSTRD(cap);
    int to    = (int)NVME_CAP_TIMEOUT(cap);

    nvme_ctrl.doorbell_stride = 4 << dstrd;
    nvme_ctrl.timeout_500ms = to;
    nvme_ctrl.q_depth = (mqes + 1 < 64) ? mqes + 1 : 64;

    kprintf("[NVMe] v%d.%d, MQES=%d stride=%d timeout=%d qdepth=%d\n",
            (vs >> 16) & 0xFF, vs & 0xFFFF, mqes, dstrd, to, nvme_ctrl.q_depth);

    nvme_ctrl.asq = (nvme_cmd_t*)nvme_alloc_page(&nvme_ctrl.asq_phys);
    nvme_ctrl.acq = (nvme_cpl_t*)nvme_alloc_page(&nvme_ctrl.acq_phys);
    if (!nvme_ctrl.asq || !nvme_ctrl.acq) return ERR_NOMEM;
    kmemset(nvme_ctrl.asq, 0, 4096);
    kmemset(nvme_ctrl.acq, 0, 4096);

    nvme_ctrl.acq_phase = 1;

    nvme_ctrl.regs[NVME_AQA / 4] = NVME_AQA_ASQS(nvme_ctrl.q_depth - 1)
                                  | NVME_AQA_ACQS(nvme_ctrl.q_depth - 1);
    nvme_write64(nvme_ctrl.regs, NVME_ASQ, nvme_ctrl.asq_phys);
    nvme_write64(nvme_ctrl.regs, NVME_ACQ, nvme_ctrl.acq_phys);
    asm volatile("mfence" ::: "memory");

    uint32_t cc = NVME_CC_EN | NVME_CC_CSS(0) | NVME_CC_MPS(0)
                | NVME_CC_IOSQES(6) | NVME_CC_IOCQES(4);
    nvme_ctrl.regs[NVME_CC / 4] = cc;

    if (nvme_wait_bit(nvme_ctrl.regs + NVME_CSTS / 4, NVME_CSTS_RDY, 1, to * 500)) {
        kprintf("[NVMe] Controller not ready\n");
        return ERR_IO;
    }
    kprintf("[NVMe] Ready\n");

    {
        uint64_t bp;
        void* buf = nvme_alloc_page(&bp);
        if (!buf) return ERR_NOMEM;
        nvme_cmd_t cmd = { .opcode = NVME_ADM_IDENTIFY, .cid = nvme_next_cid(), .cdw10 = NVME_IDENTIFY_CTRL };
        if (nvme_admin_cmd(&cmd, bp) == 0) {
            nvme_ctrl_data_t* cd = (nvme_ctrl_data_t*)buf;
            kprintf("[NVMe] %.40s FW:%.8s SN:%.20s\n", cd->mn, cd->fr, cd->sn);
        }
        nvme_free_page(buf, bp);
    }

    {
        uint64_t bp;
        void* buf = nvme_alloc_page(&bp);
        if (!buf) return ERR_NOMEM;
        nvme_cmd_t cmd = { .opcode = NVME_ADM_IDENTIFY, .cid = nvme_next_cid(), .nsid = 1, .cdw10 = NVME_IDENTIFY_NS };
        if (nvme_admin_cmd(&cmd, bp) == 0) {
            nvme_ns_data_t* ns = (nvme_ns_data_t*)buf;
            nvme_ctrl.nsze = ns->nsze;
            uint32_t flbas = ns->flbas & 0x0F;
            uint32_t lbads = *(uint32_t*)&ns->lba_format[flbas * 4] & 0xFFFF;
            nvme_ctrl.lba_shift = (lbads > 0) ? lbads : 9;
            kprintf("[NVMe] NS1: %llu sectors, LBA %u bytes\n", ns->nsze, 1U << nvme_ctrl.lba_shift);
        } else {
            kprintf("[NVMe] Identify NS failed\n");
        }
        nvme_free_page(buf, bp);
    }

    if (nvme_ctrl.nsze == 0) {
        kprintf("[NVMe] No active namespace\n");
        return ERR_NOENT;
    }

    nvme_bounce = nvme_alloc_page(&nvme_bounce_phys);
    if (!nvme_bounce) return ERR_NOMEM;

    {
        uint64_t bp;
        nvme_ctrl.iocq = (nvme_cpl_t*)nvme_alloc_page(&bp);
        if (!nvme_ctrl.iocq) return ERR_NOMEM;
        nvme_ctrl.iocq_phys = bp;
        nvme_ctrl.iocq_phase = 1;
        kmemset(nvme_ctrl.iocq, 0, 4096);

        nvme_cmd_t cmd = {
            .opcode = NVME_ADM_CREATE_IOCQ,
            .cid = nvme_next_cid(),
            .cdw10 = (uint32_t)((nvme_ctrl.q_depth - 1) | (1 << 16)),
            .cdw11 = 1,
            .prp1 = nvme_ctrl.iocq_phys,
        };
        if (nvme_admin_cmd(&cmd, 0) != 0) {
            kprintf("[NVMe] Create IOCQ failed\n");
            return ERR_IO;
        }
    }

    {
        uint64_t bp;
        nvme_ctrl.iosq = (nvme_cmd_t*)nvme_alloc_page(&bp);
        if (!nvme_ctrl.iosq) return ERR_NOMEM;
        nvme_ctrl.iosq_phys = bp;
        kmemset(nvme_ctrl.iosq, 0, 4096);

        nvme_cmd_t cmd = {
            .opcode = NVME_ADM_CREATE_IOSQ,
            .cid = nvme_next_cid(),
            .cdw10 = (uint32_t)((nvme_ctrl.q_depth - 1) | (1 << 16) | (1 << 17)),
            .cdw11 = (uint32_t)(1 | (1 << 16)),
            .prp1 = nvme_ctrl.iosq_phys,
        };
        if (nvme_admin_cmd(&cmd, 0) != 0) {
            kprintf("[NVMe] Create IOSQ failed\n");
            return ERR_IO;
        }
    }

    {
        block_dev_t bd;
        kmemset(&bd, 0, sizeof(bd));
        kstrncpy(bd.name, "nvme0", sizeof(bd.name) - 1);
        bd.block_count = nvme_ctrl.nsze;
        bd.block_size = 1U << nvme_ctrl.lba_shift;
        bd.read = nvme_blk_read;
        bd.write = nvme_blk_write;
        bd.private_data = NULL;

        int idx = block_register(&bd);
        if (idx >= 0) {
            kprintf("[NVMe] Registered 'nvme0' (%llu blocks, %u bytes)\n",
                    bd.block_count, bd.block_size);
        }
    }

    nvme_ctrl.present = 1;
    kprintf("[NVMe] Initialized\n");
    return ERR_OK;
}
