#ifndef ACPI_H
#define ACPI_H

#include "types.h"

/* RSDP (Root System Description Pointer) */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI v2.0+ extended fields */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;

/* SDT header (RSDT/XSDT/MADT/etc) */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

/* MADT entry header */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

/* MADT - Processor Local APIC (type 0) */
typedef struct {
    madt_entry_header_t hdr;
    uint8_t  acpi_processor_uid;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_processor_entry_t;

/* MADT - I/O APIC (type 1) */
typedef struct {
    madt_entry_header_t hdr;
    uint8_t  io_apic_id;
    uint8_t  reserved;
    uint32_t io_apic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_io_apic_entry_t;

/* MADT - Interrupt Source Override (type 2) */
typedef struct {
    madt_entry_header_t hdr;
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) madt_iso_entry_t;

/* MADT - Local x2APIC (type 9) */
typedef struct {
    madt_entry_header_t hdr;
    uint16_t reserved;
    uint32_t apic_id;
    uint32_t flags;
    uint32_t acpi_processor_uid;
} __attribute__((packed)) madt_x2apic_entry_t;

/* CPU info, filled by acpi_parse_madt */
#define MAX_CPUS 64

typedef struct {
    uint32_t apic_id;      /* xAPIC or x2APIC ID */
    uint32_t processor_uid;
    uint8_t  flags;        /* bit 0: enabled */
    uint8_t  is_bsp;       /* 1 for bootstrap processor */
} cpu_info_t;

extern int acpi_available;
extern int cpu_count;
extern cpu_info_t cpu_info[MAX_CPUS];
extern int nr_cpus;

err_t acpi_init(uint64_t mb_info_phys);
int   acpi_parse_madt(void);
void  acpi_scan_cpus(void);

/* I/O APIC info from MADT */
#define MAX_IO_APICS 8

typedef struct {
    uint8_t  id;
    uint32_t address;      /* physical MMIO base */
    uint32_t gsi_base;
} io_apic_info_t;

extern int io_apic_count;
extern io_apic_info_t io_apics[MAX_IO_APICS];

/* Interrupt Source Overrides */
#define MAX_ISOS 16

typedef struct {
    uint8_t  bus;
    uint8_t  source;       /* ISA IRQ */
    uint32_t gsi;          /* Global System Interrupt */
    uint16_t flags;        /* Polarity, Trigger mode */
} iso_entry_t;

extern int iso_count;
extern iso_entry_t isos[MAX_ISOS];

/* ---- NUMA (SRAT / SLIT) ---- */

#define MAX_NUMA_NODES 8
#define MAX_MEMORY_AFFINITIES 32
#define MAX_CPU_AFFINITIES 64

/* SRAT - Memory Affinity (type 1) */
typedef struct __attribute__((packed)) {
    madt_entry_header_t hdr;
    uint32_t proximity_domain;
    uint16_t reserved1;
    uint64_t base_addr;
    uint64_t length;
    uint32_t reserved2;
    uint32_t flags;  /* bit 0 = enabled */
} srat_memory_affinity_t;

/* SRAT - LAPIC Affinity (type 0) */
typedef struct __attribute__((packed)) {
    madt_entry_header_t hdr;
    uint8_t  proximity_domain;
    uint8_t  apic_id;
    uint32_t flags;  /* bit 0 = enabled */
} srat_lapic_affinity_t;

/* SRAT - x2APIC Affinity (type 2) */
typedef struct __attribute__((packed)) {
    madt_entry_header_t hdr;
    uint16_t reserved;
    uint32_t proximity_domain;
    uint32_t apic_id;
    uint32_t flags;  /* bit 0 = enabled */
} srat_x2apic_affinity_t;

/* Parsed memory region — maps a physical range to a NUMA node */
typedef struct {
    uint64_t base;
    uint64_t length;
    int node;
    int enabled;
} numa_memory_region_t;

extern int numa_available;
extern int numa_node_count;
extern numa_memory_region_t numa_memory_regions[MAX_MEMORY_AFFINITIES];
extern int numa_memory_region_count;
extern int numa_cpu_to_node[MAX_CPUS];
extern uint8_t numa_distance[MAX_NUMA_NODES][MAX_NUMA_NODES];

int acpi_parse_srat(void);
int acpi_parse_slit(void);
int acpi_get_cpu_node(int cpu_idx);

/* Get NUMA distance (0=undefined, 10=self, larger=farther).
 * Returns 10 for local, or the SLIT value if available. */
int acpi_node_distance(int from, int to);

/* Check if a physical page at page_idx belongs to the given NUMA node.
 * Returns 1 if yes, 0 if no or if NUMA is not available. */
int acpi_is_page_in_node(uint64_t page_idx, int node);

#endif /* ACPI_H */
