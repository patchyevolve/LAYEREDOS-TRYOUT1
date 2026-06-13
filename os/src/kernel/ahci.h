#ifndef AHCI_H
#define AHCI_H

#include "types.h"
#include "sync.h"

#define AHCI_MAX_PORTS 32
#define AHCI_CMD_SLOTS 32
#define AHCI_SECTOR_SIZE 512
#define AHCI_MAX_DEVICES 32

/* HBA registers (offset from ABAR) */
#define AHCI_CAP  0x00
#define AHCI_GHC  0x04
#define AHCI_IS   0x08
#define AHCI_PI   0x0C
#define AHCI_VS   0x10

/* GHC bits */
#define AHCI_GHC_AE     (1 << 31)
#define AHCI_GHC_IE     (1 << 1)
#define AHCI_GHC_HR     (1 << 0)

/* Port registers (offset = 0x100 + port * 0x80) */
#define AHCI_PxCLB   0x00
#define AHCI_PxCLBU  0x04
#define AHCI_PxFB    0x08
#define AHCI_PxFBU   0x0C
#define AHCI_PxIS    0x10
#define AHCI_PxIE    0x14
#define AHCI_PxCMD   0x18
#define AHCI_PxTFD   0x20
#define AHCI_PxSIG   0x24
#define AHCI_PxSSTS  0x28
#define AHCI_PxSCTL  0x2C
#define AHCI_PxSERR  0x30
#define AHCI_PxCI    0x38
#define AHCI_PxSNTF  0x3C

/* PxCMD bits */
#define AHCI_PxCMD_ST     (1 << 0)
#define AHCI_PxCMD_FRE    (1 << 4)
#define AHCI_PxCMD_FR     (1 << 14)
#define AHCI_PxCMD_CR     (1 << 15)
#define AHCI_PxCMD_CPD    (1 << 7)
#define AHCI_PxCMD_ESP    (1 << 8)
#define AHCI_PxCMD_ATAPI  (1 << 24)
#define AHCI_PxCMD_ICC_MASK (0xF << 28)
#define AHCI_PxCMD_ICC_ACTIVE (1 << 28)

/* PxSIG values */
#define AHCI_SIG_ATA  0x00000101
#define AHCI_SIG_ATAPI 0xEB140101
#define AHCI_SIG_PM   0x96690101
#define AHCI_SIG_SEMB 0xC33C0101

/* PxSSTS bits */
#define AHCI_PxSSTS_DET_MASK  0x0F
#define AHCI_PxSSTS_DET_NONE  0x00
#define AHCI_PxSSTS_DET_PRES  0x03
#define AHCI_PxSSTS_DET_ACTIVE 0x07
#define AHCI_PxSSTS_IPM_MASK  0x0F00
#define AHCI_PxSSTS_IPM_ACTIVE 0x0100

/* Command list entry (32 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t cfis_length : 5;
    uint16_t atapi       : 1;
    uint16_t write       : 1;
    uint16_t prefetch    : 1;
    uint16_t reset       : 1;
    uint16_t bist        : 1;
    uint16_t clear_bsy   : 1;
    uint16_t reserved0   : 1;
    uint16_t port_mult   : 4;
    uint16_t reserved1   : 16;
    uint32_t prdtl;       /* Physical Region Descriptor Table Length */
    volatile uint32_t prdbc;  /* Physical Region Descriptor Byte Count */
    uint32_t ctba;        /* Command Table Base Address (lower) */
    uint32_t ctbau;       /* Command Table Base Address (upper) */
    uint32_t reserved2[4];
} ahci_cmd_hdr_t;

/* Command table (128 bytes + PRDT entries) */
typedef struct __attribute__((packed)) {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    /* PRDT entries follow */
} ahci_cmd_table_t;

/* PRD entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dba;       /* Data Base Address (lower) */
    uint32_t dbau;      /* Data Base Address (upper) */
    uint32_t reserved0;
    uint32_t dbc;       /* Byte count (22-bit, bit 31 = interrupt on completion) */
} ahci_prdt_entry_t;

/* Received FIS structure (256 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t dma_setup[28];
    uint8_t pio_setup[28];
    uint8_t d2h_reg[28];
    uint8_t sdb[16];
    uint8_t ufis[64];
    uint8_t reserved[96];
} ahci_fis_t;

/* Port data */
typedef struct {
    int         present;
    uint32_t    port_num;
    uint64_t    sector_count;
    char        model[41];

    ahci_cmd_hdr_t* cmd_list;
    uint64_t    cmd_list_phys;
    ahci_fis_t* fis;
    uint64_t    fis_phys;
    ahci_cmd_table_t* cmd_table;
    uint64_t    cmd_table_phys;
} ahci_port_t;

/* AHCI controller data */
typedef struct {
    int         present;
    volatile uint32_t* abar;   /* MMIO base */
    uint64_t    abar_phys;
    uint32_t    ports_impl;
    int         n_ports;
    ahci_port_t ports[AHCI_MAX_PORTS];
    spinlock_t  lock;
} ahci_ctrl_t;

err_t ahci_init(void);

#endif
