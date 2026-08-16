#ifndef E1000_H
#define E1000_H

#include "types.h"

/* ── PCI Vendor/Device IDs ──────────────────────────────────────────────── */
#define E1000_VENDOR_INTEL  0x8086
#define E1000_DEV_82540EM   0x100E   /* QEMU default 'e1000' */
#define E1000_DEV_82545EM   0x100F
#define E1000_DEV_82571EB   0x105E   /* e1000e family */
#define E1000_DEV_82572EI   0x107D   /* e1000e family */
#define E1000_DEV_82576     0x10C9   /* QEMU 'igb' */
#define E1000_DEV_82573L    0x109A   /* e1000e family */
#define E1000_DEV_82574L    0x10D3   /* QEMU 'e1000e' */
#define E1000_DEV_82579V    0x1503   /* e1000e family */
#define E1000_DEV_I350      0x1521   /* igb family */
#define E1000_DEV_I210      0x1533   /* igb family */
#define E1000_DEV_I211      0x1539   /* igb family */
#define E1000_DEV_I217V     0x15B8   /* e1000e family */
#define E1000_DEV_I218V     0x15A1   /* e1000e family */
#define E1000_DEV_I219V     0x15BC   /* e1000e family (modern consumer) */

/* ── Controller model families ──────────────────────────────────────────── */
typedef enum {
    E1000_MODEL_LEGACY = 0,   /* 82540EM/82545EM: 82540-class register set */
    E1000_MODEL_E1000E,       /* 82571/82572/82573/82574/82579/I217/I218/I219 */
    E1000_MODEL_IGB,          /* 82576/I350/I210/I211 */
} e1000_model_t;

const char* e1000_model_name(e1000_model_t m);
e1000_model_t e1000_model_for_devid(uint16_t devid);

/* ── MMIO Register Offsets ──────────────────────────────────────────────── */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EECD      0x0010
#define E1000_EERD      0x0014
#define E1000_ICS       0x00C8
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_RDTR      0x2820
#define E1000_RXDCTL    0x3828
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_TXDCTL    0x3828
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404
#define E1000_MTA       0x5200
#define E1000_MANC      0x5820   /* Management Control */
#define E1000_IVAR      0x1700   /* IVAR (real 82574; entries: valid=0x80) */
#define E1000_IVAR_QEMU 0x00E4   /* QEMU e1000e IVAR (entries: valid=0x8) */
/* Statistics registers (from Intel 82540EM / QEMU e1000x_regs.h) */
#define E1000_GPRC      0x4074   /* Good Packets Received Count - R/clr */
#define E1000_GPTC      0x4080   /* Good Packets Transmitted Count - R/clr */
#define E1000_GORCL     0x4088   /* Good Octets Received Count Low - R/clr */
#define E1000_GORCH     0x408C   /* Good Octets Received Count High - R/clr */
#define E1000_TPR       0x40D0   /* Total Packets Received - R/clr */
#define E1000_TPT       0x40D4   /* Total Packets Transmitted - R/clr */
#define E1000_ROC       0x40AC   /* Receive Oversize Count - R/clr */
#define E1000_RUC       0x40A4   /* Receive Undersize Count - R/clr */
#define E1000_RJC       0x40B0   /* Receive Jabber Count - R/clr */
#define E1000_RNBC      0x40A0   /* Receive No Buffers Count - R/clr */

/* ── CTRL register bits ─────────────────────────────────────────────────── */
#define E1000_CTRL_FD       (1 << 0)
#define E1000_CTRL_LRST     (1 << 3)
#define E1000_CTRL_ASDE     (1 << 5)
#define E1000_CTRL_SLU      (1 << 6)
#define E1000_CTRL_ILOS     (1 << 7)
#define E1000_CTRL_RST      (1 << 26)
#define E1000_CTRL_VME      (1 << 30)
#define E1000_CTRL_PHY_RST  (1 << 31)

/* ── CTRL_EXT register (e1000e/igb, offset 0x18) ───────────────────────── */
#define E1000_CTRL_EXT      0x0018
#define E1000_CTRL_EXT_EXTDE (1 << 30)  /* Extended descriptors; 0 = legacy */

/* ── TIPG register (e1000e/igb, offset 0x0410) ─────────────────────────── */
#define E1000_TIPG          0x0410
/* Linux e1000e: IPGT=8, IPGR1=8, IPGR2=2 for 1 Gb/s links */
#define E1000_TIPG_1000     0x0A0806

/* ── MDIC register (e1000e/igb, offset 0x20) — PHY access ─────────────── */
#define E1000_MDIC          0x0020
#define E1000_MDIC_DATA_MASK 0x0000FFFF
#define E1000_MDIC_REG_SHIFT 16
#define E1000_MDIC_REG_MASK  0x001F0000
#define E1000_MDIC_PHY_SHIFT 21
#define E1000_MDIC_PHY_MASK  0x03E00000
#define E1000_MDIC_OP_READ   0x04000000
#define E1000_MDIC_OP_WRITE  0x08000000
#define E1000_MDIC_READY     0x10000000
#define E1000_MDIC_ERROR     0x20000000

/* ── MII PHY BMCR bits (written via MDIC) ─────────────────────────────── */
#define E1000_BMCR_SPEED1000 0x0040
#define E1000_BMCR_FD        0x0100
#define E1000_BMCR_ANRESTART 0x0200
#define E1000_BMCR_AUTOEN    0x1000

/* ── Advanced (extended) TX descriptor bits (igb family) ──────────────── */
#define E1000_ADVTXD_DTYP_CTXT 0x00200000
#define E1000_ADVTXD_DTYP_DATA 0x00300000
#define E1000_ADVTXD_DCMD_EOP  0x01000000
#define E1000_ADVTXD_DCMD_IFCS 0x02000000
#define E1000_ADVTXD_DCMD_RS   0x08000000
#define E1000_ADVTXD_DCMD_DEXT 0x20000000

/* ── SRRCTL0 (igb): Rx descriptor type, shares offset 0x100 with RCTL ─── */
#define E1000_SRRCTL_DESCTYPE_ADV_ONEBUF 0x02000000

/* ── STATUS register bits ───────────────────────────────────────────────── */
#define E1000_STATUS_FD      (1 << 0)
#define E1000_STATUS_LU      (1 << 1)
#define E1000_STATUS_SPEED_MASK (0xC0)
#define E1000_STATUS_SPEED_10   (0x00)
#define E1000_STATUS_SPEED_100  (0x40)
#define E1000_STATUS_SPEED_1000 (0x80)

/* ── EECD register bits (EEPROM) ────────────────────────────────────────── */
#define E1000_EECD_SK       (1 << 0)
#define E1000_EECD_CS       (1 << 1)
#define E1000_EECD_DI       (1 << 2)
#define E1000_EECD_DO       (1 << 3)
#define E1000_EECD_REQ      (1 << 6)
#define E1000_EECD_GNT      (1 << 7)
#define E1000_EECD_PRES     (1 << 8)

/* ── EERD register bits (EEPROM read) ───────────────────────────────────── */
#define E1000_EERD_START    (1 << 0)
#define E1000_EERD_DONE     (1 << 4)
#define E1000_EERD_ADDR_SH  8
#define E1000_EERD_DATA_SH  16

/* ── RCTL register bits ─────────────────────────────────────────────────── */
#define E1000_RCTL_EN       (1 << 1)
#define E1000_RCTL_SBP      (1 << 2)
#define E1000_RCTL_UPE      (1 << 3)
#define E1000_RCTL_MPE      (1 << 4)
#define E1000_RCTL_LPE      (1 << 5)
#define E1000_RCTL_LBM_NONE (0 << 6)
#define E1000_RCTL_LBM_PHY  (3 << 6)
#define E1000_RCTL_RDMTS_HALF (0 << 8)
#define E1000_RCTL_RDMTS_QUARTER (1 << 8)
#define E1000_RCTL_MO_SH    12
#define E1000_RCTL_BAM      (1 << 15)
#define E1000_RCTL_BSIZE_256   (3 << 16)
#define E1000_RCTL_BSIZE_512   (2 << 16)
#define E1000_RCTL_BSIZE_1024  (1 << 16)
#define E1000_RCTL_BSIZE_2048  (0 << 16)
#define E1000_RCTL_SECRC    (1 << 26)

/* ── TCTL register bits ─────────────────────────────────────────────────── */
#define E1000_TCTL_EN       (1 << 1)
#define E1000_TCTL_PSP      (1 << 3)
#define E1000_TCTL_CT_SH    4
#define E1000_TCTL_COLD_SH  12
#define E1000_TCTL_SWXOFF   (1 << 22)
#define E1000_TCTL_RTLC     (1 << 24)

/* ── Interrupt bits ─────────────────────────────────────────────────────── */
#define E1000_ICR_TXDW      (1 << 0)
#define E1000_ICR_TXQE      (1 << 1)
#define E1000_ICR_LSC       (1 << 2)
#define E1000_ICR_RXDMT0    (1 << 4)
#define E1000_ICR_RXT0      (1 << 7)
#define E1000_ICR_RXQ0      (1 << 20)   /* e1000e: per-queue RX cause (QEMU/Intel) */

/* ── TX descriptor command bits ─────────────────────────────────────────── */
#define E1000_TXD_CMD_EOP    (1 << 0)
#define E1000_TXD_CMD_IFCS   (1 << 1)
#define E1000_TXD_CMD_IC     (1 << 2)
#define E1000_TXD_CMD_RS     (1 << 3)
#define E1000_TXD_CMD_RPS    (1 << 4)
#define E1000_TXD_CMD_DEXT   (1 << 5)
#define E1000_TXD_CMD_VLE    (1 << 6)
#define E1000_TXD_CMD_IDE    (1 << 7)

/* ── TX descriptor status bits ──────────────────────────────────────────── */
#define E1000_TXD_STAT_DD    (1 << 0)
#define E1000_TXD_STAT_EC    (1 << 1)
#define E1000_TXD_STAT_LC    (1 << 2)

/* ── RX descriptor status bits ──────────────────────────────────────────── */
#define E1000_RXD_STAT_DD    (1 << 0)
#define E1000_RXD_STAT_EOP   (1 << 1)
#define E1000_RXD_STAT_IXSM  (1 << 2)
#define E1000_RXD_STAT_VP    (1 << 3)

/* ── Descriptor ring sizes ──────────────────────────────────────────────── */
#define E1000_NUM_RX_DESC    32
#define E1000_NUM_TX_DESC    32
#define E1000_RX_BUF_SIZE    2048
#define E1000_TX_BUF_SIZE    2048

/* ── Descriptor structures ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

/* ── Advanced descriptor structures (igb family, 16 bytes each) ────────── */
typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;    /* offset 0 */
    uint32_t cmd_type_len;   /* offset 8: len[15:0] | DEXT | DTYP=DATA | EOP | IFCS | RS */
    uint32_t olinfo_status;  /* offset 12: DD=bit0 written back by HW (whole desc zeroed) */
} e1000_adv_tx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t pkt_addr;       /* offset 0 */
    uint32_t status_error;   /* offset 8: DD=bit0, EOP=bit1 (writeback) */
    uint16_t length;         /* offset 12 (writeback) */
    uint16_t vlan;           /* offset 14 (writeback) */
} e1000_adv_rx_desc_t;

/* Public API for debugging */
uint32_t nic_reg_read(uint32_t off);
void     nic_reg_write(uint32_t off, uint32_t val);
void     nic_dump_rx_ring(void);
void     nic_dump_tx_ring(void);
void     e1000_mta_set(const uint8_t* mac);

/* MSI-X state (used by kernel self-tests) */
int      e1000_msix_active(void);
uint32_t e1000_msix_count(void);

#endif
