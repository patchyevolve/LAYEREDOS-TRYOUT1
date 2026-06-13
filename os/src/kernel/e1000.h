#ifndef E1000_H
#define E1000_H

#include "types.h"

/* ── PCI Vendor/Device IDs ──────────────────────────────────────────────── */
#define E1000_VENDOR_INTEL  0x8086
#define E1000_DEV_82540EM   0x100E
#define E1000_DEV_82545EM   0x100F
#define E1000_DEV_82573L    0x109A
#define E1000_DEV_82574L    0x10D3

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

/* Public API for debugging */
uint32_t nic_reg_read(uint32_t off);
void     nic_reg_write(uint32_t off, uint32_t val);
void     nic_dump_rx_ring(void);
void     nic_dump_tx_ring(void);
void     e1000_mta_set(const uint8_t* mac);

#endif
