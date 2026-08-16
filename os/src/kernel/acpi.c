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

/* NUMA globals */
int numa_available = 0;
int numa_node_count = 0;
numa_memory_region_t numa_memory_regions[MAX_MEMORY_AFFINITIES];
int numa_memory_region_count = 0;
int numa_cpu_to_node[MAX_CPUS];
uint8_t numa_distance[MAX_NUMA_NODES][MAX_NUMA_NODES];

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

/* ---- NUMA Support (SRAT/SLIT) ---- */

/* Initialize NUMA structures */
static void numa_init_globals(void) {
    for (int i = 0; i < MAX_CPUS; i++)
        numa_cpu_to_node[i] = -1;
    numa_memory_region_count = 0;
    numa_node_count = 0;
    numa_available = 0;
}

/* Find the highest node index among parsed regions */
static void numa_update_node_count(void) {
    int max_node = -1;
    for (int i = 0; i < numa_memory_region_count; i++) {
        if (numa_memory_regions[i].enabled &&
            numa_memory_regions[i].node > max_node)
            max_node = numa_memory_regions[i].node;
    }
    numa_node_count = max_node + 1;
}

/* Parse SRAT (Static Resource Affinity Table):
 * Walks the XSDT/RSDT looking for "SRAT", then processes:
 *   - Type 0: LAPIC affinity (CPU → node)
 *   - Type 1: Memory affinity (physical range → node)
 */
int acpi_parse_srat(void) {
    if (!acpi_available) return -1;

    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) return -1;

    uint32_t entry_count;
    sdt_header_t* root_table;
    int use_xsdt = 0;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root_table = acpi_map_table(rsdp->xsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 8;
        use_xsdt = 1;
    } else if (rsdp->rsdt_addr) {
        root_table = acpi_map_table(rsdp->rsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 4;
    } else {
        return -1;
    }

    if (acpi_checksum(root_table, root_table->length) != 0)
        return -1;

    numa_init_globals();

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_phys;
        if (use_xsdt) {
            uint64_t* entries = (uint64_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        } else {
            uint32_t* entries = (uint32_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        }

        sdt_header_t* tbl = acpi_map_table(entry_phys);
        if (!tbl) continue;

        if (tbl->signature[0] == 'S' && tbl->signature[1] == 'R' &&
            tbl->signature[2] == 'A' && tbl->signature[3] == 'T') {
            if (acpi_checksum(tbl, tbl->length) != 0) {
                kprintf("[ACPI] SRAT checksum failed\n");
                continue;
            }

            kprintf("[ACPI] SRAT found: length=%u, revision=%u\n",
                    tbl->length, (uint32_t)tbl->revision);

            /* SRAT header: standard SDT header + reserved[12] at offset 36 */
            uint8_t* entry_ptr = (uint8_t*)tbl + 48; /* sizeof(sdt_header_t) + 12 */
            uint8_t* end = (uint8_t*)tbl + tbl->length;

            while (entry_ptr < end) {
                madt_entry_header_t* hdr = (madt_entry_header_t*)entry_ptr;
                if (hdr->length == 0) break;

                switch (hdr->type) {
                case 0: { /* LAPIC Affinity */
                    srat_lapic_affinity_t* la = (srat_lapic_affinity_t*)entry_ptr;
                    if (la->flags & 1) {
                        int apic_id = la->apic_id;
                        int node = la->proximity_domain;
                        /* Map APIC ID to CPU index */
                        for (int c = 0; c < cpu_count; c++) {
                            if (cpu_info[c].apic_id == (uint32_t)apic_id) {
                                numa_cpu_to_node[c] = node;
                                break;
                            }
                        }
                    }
                    break;
                }
                case 2: { /* x2APIC Affinity */
                    srat_x2apic_affinity_t* xa = (srat_x2apic_affinity_t*)entry_ptr;
                    if (xa->flags & 1) {
                        uint32_t apic_id = xa->apic_id;
                        int node = (int)xa->proximity_domain;
                        for (int c = 0; c < cpu_count; c++) {
                            if (cpu_info[c].apic_id == apic_id) {
                                numa_cpu_to_node[c] = node;
                                break;
                            }
                        }
                    }
                    break;
                }
                case 1: { /* Memory Affinity */
                    srat_memory_affinity_t* ma = (srat_memory_affinity_t*)entry_ptr;
                    if ((ma->flags & 3) == 1) { /* bit 0 = enabled, bit 1 = hotpluggable */
                        int idx = numa_memory_region_count;
                        if (idx < MAX_MEMORY_AFFINITIES) {
                            numa_memory_regions[idx].base = ma->base_addr;
                            numa_memory_regions[idx].length = ma->length;
                            numa_memory_regions[idx].node = (int)ma->proximity_domain;
                            numa_memory_regions[idx].enabled = 1;
                            numa_memory_region_count++;
                        }
                    }
                    break;
                }
                default:
                    break;
                }

                entry_ptr += hdr->length;
            }

            numa_update_node_count();
            numa_available = (numa_node_count > 0) ? 1 : 0;

            kprintf("[ACPI] SRAT: %d NUMA nodes, %d memory regions",
                    numa_node_count, numa_memory_region_count);
            for (int r = 0; r < numa_memory_region_count; r++) {
                kprintf(", [%d] base=0x%llx len=0x%llx node=%d",
                        r,
                        (unsigned long long)numa_memory_regions[r].base,
                        (unsigned long long)numa_memory_regions[r].length,
                        numa_memory_regions[r].node);
            }
            kprintf("\n");

            /* Redistribute PMM free pages to per-node lists */
            pmm_numa_init();

            return numa_node_count;
        }
    }

    kprintf("[ACPI] SRAT not found — no NUMA topology\n");
    return -1;
}

int acpi_parse_slit(void) {
    if (!acpi_available) return -1;

    /* Initialize distance matrix with default values */
    int max_nodes = (numa_node_count > 0) ? numa_node_count : 1;
    for (int i = 0; i < MAX_NUMA_NODES; i++)
        for (int j = 0; j < MAX_NUMA_NODES; j++)
            numa_distance[i][j] = 0;

    /* If only 1 node, self-distance is 10 */
    if (max_nodes <= 1) {
        numa_distance[0][0] = 10;
        return 0;
    }

    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) return -1;

    uint32_t entry_count;
    sdt_header_t* root_table;
    int use_xsdt = 0;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root_table = acpi_map_table(rsdp->xsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 8;
        use_xsdt = 1;
    } else if (rsdp->rsdt_addr) {
        root_table = acpi_map_table(rsdp->rsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 4;
    } else {
        return -1;
    }

    if (acpi_checksum(root_table, root_table->length) != 0)
        return -1;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_phys;
        if (use_xsdt) {
            uint64_t* entries = (uint64_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        } else {
            uint32_t* entries = (uint32_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        }

        sdt_header_t* tbl = acpi_map_table(entry_phys);
        if (!tbl) continue;

        if (tbl->signature[0] == 'S' && tbl->signature[1] == 'L' &&
            tbl->signature[2] == 'I' && tbl->signature[3] == 'T') {
            if (acpi_checksum(tbl, tbl->length) != 0) {
                kprintf("[ACPI] SLIT checksum failed\n");
                continue;
            }

            kprintf("[ACPI] SLIT found: length=%u, revision=%u\n",
                    tbl->length, (uint32_t)tbl->revision);

            /* SLIT: header + matrix[num_nodes][num_nodes] of uint8_t distances */
            int nodes_in_slit = max_nodes;
            uint8_t* matrix = (uint8_t*)((uintptr_t)tbl + sizeof(sdt_header_t));
            int matrix_size = nodes_in_slit * nodes_in_slit;
            if ((int)tbl->length < (int)sizeof(sdt_header_t) + matrix_size) {
                kprintf("[ACPI] SLIT too short: have %u need %u\n",
                        tbl->length, (uint32_t)(sizeof(sdt_header_t) + matrix_size));
                continue;
            }

            for (int row = 0; row < nodes_in_slit && row < MAX_NUMA_NODES; row++)
                for (int col = 0; col < nodes_in_slit && col < MAX_NUMA_NODES; col++)
                    numa_distance[row][col] = matrix[row * nodes_in_slit + col];

            kprintf("[ACPI] SLIT:");
            for (int r = 0; r < nodes_in_slit && r < MAX_NUMA_NODES; r++) {
                kprintf(" node %d:", r);
                for (int c = 0; c < nodes_in_slit && c < MAX_NUMA_NODES; c++)
                    kprintf(" %d", numa_distance[r][c]);
            }
            kprintf("\n");

            return 0;
        }
    }

    /* SLIT not found: set default distance (10 self, 20 remote) */
    for (int i = 0; i < max_nodes; i++)
        for (int j = 0; j < max_nodes; j++)
            numa_distance[i][j] = (i == j) ? 10 : 20;

    return -1;
}

int acpi_node_distance(int from, int to) {
    if (!numa_available)
        return 10;
    if (from < 0 || from >= MAX_NUMA_NODES || to < 0 || to >= MAX_NUMA_NODES)
        return 0;
    uint8_t d = numa_distance[from][to];
    return (d != 0) ? (int)d : (from == to) ? 10 : 20;
}

/* Get the NUMA node for a given CPU index.
 * Returns 0 on a non-NUMA system. */
int acpi_get_cpu_node(int cpu_idx) {
    if (!numa_available || cpu_idx < 0 || cpu_idx >= cpu_count)
        return 0;
    int node = numa_cpu_to_node[cpu_idx];
    return (node >= 0) ? node : 0;
}

/* Check if a physical page at page_idx belongs to the given NUMA node.
 * page_idx is the page index (phys_addr / PAGE_SIZE).
 * Returns 1 if yes, 0 if no or if NUMA is not available. */
int acpi_is_page_in_node(uint64_t page_idx, int node) {
    if (!numa_available || node < 0)
        return 0;
    uint64_t phys_addr = page_idx * 4096ULL;  /* PAGE_SIZE = 4096 */
    for (int i = 0; i < numa_memory_region_count; i++) {
        if (numa_memory_regions[i].enabled &&
            numa_memory_regions[i].node == node &&
            phys_addr >= numa_memory_regions[i].base &&
            phys_addr < numa_memory_regions[i].base + numa_memory_regions[i].length) {
            return 1;
        }
    }
    return 0;
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

/* ---- PCIe ECAM (MCFG) ---- */

uint64_t mcfg_base_addr = 0;
int mcfg_segment = 0;
int mcfg_start_bus = 0;
int mcfg_end_bus = -1;

int acpi_parse_mcfg(void) {
    if (!acpi_available) return -1;

    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) return -1;

    uint32_t entry_count;
    sdt_header_t* root_table;
    int use_xsdt = 0;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root_table = acpi_map_table(rsdp->xsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 8;
        use_xsdt = 1;
    } else if (rsdp->rsdt_addr) {
        root_table = acpi_map_table(rsdp->rsdt_addr);
        if (!root_table) return -1;
        entry_count = (root_table->length - sizeof(sdt_header_t)) / 4;
    } else {
        return -1;
    }

    if (acpi_checksum(root_table, root_table->length) != 0)
        return -1;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_phys;
        if (use_xsdt) {
            uint64_t* entries = (uint64_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        } else {
            uint32_t* entries = (uint32_t*)((uintptr_t)root_table + sizeof(sdt_header_t));
            entry_phys = entries[i];
        }

        sdt_header_t* tbl = acpi_map_table(entry_phys);
        if (!tbl) continue;

        if (tbl->signature[0] == 'M' && tbl->signature[1] == 'C' &&
            tbl->signature[2] == 'F' && tbl->signature[3] == 'G') {
            if (acpi_checksum(tbl, tbl->length) != 0) {
                kprintf("[ACPI] MCFG checksum failed\n");
                continue;
            }

            kprintf("[ACPI] MCFG found: length=%u, revision=%u\n",
                    tbl->length, (uint32_t)tbl->revision);

            uint8_t* entry_ptr = (uint8_t*)tbl + sizeof(mcfg_header_t);
            uint8_t* end = (uint8_t*)tbl + tbl->length;

            while (entry_ptr + sizeof(mcfg_alloc_t) <= end) {
                mcfg_alloc_t* alloc = (mcfg_alloc_t*)entry_ptr;
                if (mcfg_base_addr == 0) {
                    mcfg_base_addr = alloc->base_addr;
                    mcfg_segment = alloc->pci_segment;
                    mcfg_start_bus = alloc->start_bus;
                    mcfg_end_bus = alloc->end_bus;
                }
                entry_ptr += sizeof(mcfg_alloc_t);
            }

            kprintf("[ACPI] ECAM: base=0x%llx seg=%d bus=%d-%d\n",
                    (unsigned long long)mcfg_base_addr, mcfg_segment,
                    mcfg_start_bus, mcfg_end_bus);
            return 0;
        }
    }
    return -1;
}
