#include "acpi.h"
#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"

int acpi_available = 0;
int cpu_count = 0;
cpu_info_t cpu_info[MAX_CPUS];
int nr_cpus = 0;

int io_apic_count = 0;
io_apic_info_t io_apics[MAX_IO_APICS];
int iso_count = 0;
iso_entry_t isos[MAX_ISOS];

/* Find RSDP by scanning BIOS memory area */
static rsdp_t* acpi_find_rsdp(void) {
    /* Scan the full first 1 MB for "RSD PTR " — some platforms place
     * RSDP below 0xE0000 (UEFI, newer QEMU with q35, etc.) */
    uint8_t* scan_start = (uint8_t*)(uintptr_t)PHYS_TO_VIRT((uint64_t)0);
    uint8_t* scan_end   = (uint8_t*)(uintptr_t)PHYS_TO_VIRT((uint64_t)0x100000);

    for (uint8_t* p = scan_start; p < scan_end; p += 16) {
        /* Quickly check common RSDP locations first */
        if (p[0] != 'R' || p[1] != 'S' || p[2] != 'D' || p[3] != ' ' ||
            p[4] != 'P' || p[5] != 'T' || p[6] != 'R' || p[7] != ' ')
            continue;
        return (rsdp_t*)p;
    }

    /* Also check EBDA (Extended BIOS Data Area) */
    uint16_t* ebda_seg = (uint16_t*)PHYS_TO_VIRT(0x40E);
    if (*ebda_seg) {
        uint8_t* start = (uint8_t*)(uintptr_t)PHYS_TO_VIRT((uint64_t)*ebda_seg << 4);
        uint8_t* end   = start + 1024;
        for (uint8_t* p = start; p < end; p += 16) {
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
                return (rsdp_t*)p;
            }
        }
    }

    return NULL;
}

static uint8_t acpi_checksum(const void* data, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++)
        sum += ((const uint8_t*)data)[i];
    return sum;
}

err_t acpi_init(uint64_t mb_info_phys) {
    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) {
        kprintf("[ACPI] RSDP not found — no ACPI tables\n");
        acpi_available = 0;
        return ERR_NOENT;
    }

    kprintf("[ACPI] RSDP found at %p, revision=%u\n", (void*)rsdp, (uint32_t)rsdp->revision);

    if (rsdp->revision >= 2) {
        /* Validate extended checksum */
        if (acpi_checksum(rsdp, sizeof(rsdp_t)) != 0) {
            kprintf("[ACPI] Extended RSDP checksum failed\n");
            acpi_available = 0;
            return ERR_IO;
        }
    } else if (acpi_checksum(rsdp, 20) != 0) {
        kprintf("[ACPI] RSDP checksum failed\n");
        acpi_available = 0;
        return ERR_IO;
    }

    acpi_available = 1;
    return ERR_OK;
}

static sdt_header_t* acpi_map_table(uint64_t phys_addr) {
    if (!phys_addr) return NULL;
    
    uint64_t virt = PHYS_TO_VIRT(phys_addr & PAGE_MASK);
    uint64_t pml4 = vmm_get_kernel_pml4();
    
    page_entry_t* pte = vmm_walk_pagetable(pml4, virt);
    if (!pte) {
        // Not mapped, map it!
        vmm_map_page(pml4, virt, phys_addr & PAGE_MASK, PAGE_WRITE);
    }
    
    return (sdt_header_t*)(uintptr_t)PHYS_TO_VIRT(phys_addr);
}

int acpi_parse_madt(void) {
    if (!acpi_available) return -1;

    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) return -1;

    /* Walk XSDT (preferred) or RSDT */
    uint32_t entry_count;
    sdt_header_t* root_table;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root_table = acpi_map_table(rsdp->xsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 8;
    } else if (rsdp->rsdt_addr) {
        root_table = acpi_map_table(rsdp->rsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 4;
    } else {
        return -1;
    }

    /* Validate root table checksum */
    if (acpi_checksum(root_table, root_table->length) != 0) {
        kprintf("[ACPI] Root table checksum failed\n");
        return -1;
    }

    /* Scan for MADT */
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_phys;
        if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
            uint64_t* entries = (uint64_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        } else {
            uint32_t* entries = (uint32_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        }

        sdt_header_t* tbl = acpi_map_table(entry_phys);
        if (!tbl) continue;

        if (tbl->signature[0] == 'A' && tbl->signature[1] == 'P' &&
            tbl->signature[2] == 'I' && tbl->signature[3] == 'C') {
            /* Validate MADT checksum */
            if (acpi_checksum(tbl, tbl->length) != 0) {
                kprintf("[ACPI] MADT checksum failed\n");
                continue;
            }

            kprintf("[ACPI] MADT found: length=%u, revision=%u\n",
                    tbl->length, (uint32_t)tbl->revision);

            cpu_count = 0;
            io_apic_count = 0;
            iso_count = 0;

            /* Parse MADT entries starting after header + local_apic_addr + flags */
            uint8_t* entry_ptr = (uint8_t*)tbl + sizeof(sdt_header_t) + 8;
            uint8_t* end = (uint8_t*)tbl + tbl->length;

            while (entry_ptr < end) {
                madt_entry_header_t* hdr = (madt_entry_header_t*)entry_ptr;
                if (hdr->length == 0) break;

                switch (hdr->type) {
                case 0: { /* Processor Local APIC */
                    madt_processor_entry_t* p = (madt_processor_entry_t*)entry_ptr;
                    if (cpu_count < MAX_CPUS) {
                        cpu_info[cpu_count].apic_id = p->apic_id;
                        cpu_info[cpu_count].processor_uid = p->acpi_processor_uid;
                        cpu_info[cpu_count].flags = p->flags & 1;
                        cpu_info[cpu_count].is_bsp = (cpu_count == 0) ? 1 : 0;
                        cpu_count++;
                    }
                    break;
                }
                case 1: { /* I/O APIC */
                    madt_io_apic_entry_t* io = (madt_io_apic_entry_t*)entry_ptr;
                    if (io_apic_count < MAX_IO_APICS) {
                        io_apics[io_apic_count].id       = io->io_apic_id;
                        io_apics[io_apic_count].address  = io->io_apic_addr;
                        io_apics[io_apic_count].gsi_base = io->gsi_base;
                        io_apic_count++;
                    }
                    break;
                }
                case 2: { /* Interrupt Source Override */
                    madt_iso_entry_t* iso = (madt_iso_entry_t*)entry_ptr;
                    if (iso_count < MAX_ISOS) {
                        isos[iso_count].bus    = iso->bus;
                        isos[iso_count].source = iso->source;
                        isos[iso_count].gsi    = iso->gsi;
                        isos[iso_count].flags  = iso->flags;
                        iso_count++;
                    }
                    break;
                }
                case 9: { /* Local x2APIC */
                    madt_x2apic_entry_t* x2 = (madt_x2apic_entry_t*)entry_ptr;
                    if (cpu_count < MAX_CPUS) {
                        cpu_info[cpu_count].apic_id = x2->apic_id;
                        cpu_info[cpu_count].processor_uid = x2->acpi_processor_uid;
                        cpu_info[cpu_count].flags = x2->flags & 1;
                        cpu_info[cpu_count].is_bsp = (cpu_count == 0) ? 1 : 0;
                        cpu_count++;
                    }
                    break;
                }
                default:
                    break;
                }

                entry_ptr += hdr->length;
            }

            nr_cpus = cpu_count;
            kprintf("[ACPI] MADT: %d CPUs, %d I/O APICs, %d ISOs\n",
                    cpu_count, io_apic_count, iso_count);

            for (int j = 0; j < cpu_count; j++) {
                kprintf("[ACPI]   CPU %d: APIC ID=%u, enabled=%d, BSP=%d\n",
                        j, cpu_info[j].apic_id, cpu_info[j].flags, cpu_info[j].is_bsp);
            }

            for (int j = 0; j < io_apic_count; j++) {
                kprintf("[ACPI]   I/O APIC %d: ID=%u, addr=0x%x, GSI base=%u\n",
                        j, io_apics[j].id, io_apics[j].address, io_apics[j].gsi_base);
            }

            return cpu_count;
        }
    }

    kprintf("[ACPI] MADT not found\n");
    return -1;
}

void acpi_scan_cpus(void) {
    if (acpi_parse_madt() <= 0) {
        /* Fallback: at least BSP is present */
        nr_cpus = 1;
        cpu_count = 1;
        kmemset(cpu_info, 0, sizeof(cpu_info));
        /* Read BSP APIC ID */
        uint64_t apic_base_msr;
        asm volatile("rdmsr" : "=a"(((uint32_t*)&apic_base_msr)[0]),
                               "=d"(((uint32_t*)&apic_base_msr)[1])
                     : "c"((uint32_t)0x1B));
        cpu_info[0].apic_id = (uint32_t)(apic_base_msr >> 12) & 0xFF;
        if (!cpu_info[0].apic_id) {
            /* Couldn't get it; assume 0 */
        }
        cpu_info[0].flags = 1;
        cpu_info[0].is_bsp = 1;
        cpu_info[0].processor_uid = 0;
        kprintf("[ACPI] No MADT, using single CPU (BSP APIC ID=%u)\n",
                (uint32_t)cpu_info[0].apic_id);
    }
}
