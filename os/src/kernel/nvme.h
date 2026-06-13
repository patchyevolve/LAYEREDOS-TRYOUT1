#ifndef NVME_H
#define NVME_H

#include "types.h"
#include "sync.h"

#define NVME_SECTOR_SIZE 512
#define NVME_MAX_QUEUE_ENTRIES 64
#define NVME_PAGE_SIZE 4096

/* Controller registers (BAR0, offset in bytes) */
#define NVME_CAP     0x00
#define NVME_VS      0x08
#define NVME_INTMS   0x0C
#define NVME_INTMC   0x10
#define NVME_CC      0x14
#define NVME_CSTS    0x1C
#define NVME_AQA     0x24
#define NVME_ASQ     0x28
#define NVME_ACQ     0x30

/* CAP bits */
#define NVME_CAP_MQES(v)     ((v) & 0xFFFF)
#define NVME_CAP_DSTRD(v)    (((v) >> 32) & 0x0F)
#define NVME_CAP_TIMEOUT(v)  (((v) >> 24) & 0xFF)
#define NVME_CAP_MPSMIN(v)   (((v) >> 48) & 0x0F)

/* CC bits */
#define NVME_CC_EN       (1 << 0)
#define NVME_CC_CSS(v)   (((v) & 0x7) << 4)
#define NVME_CC_MPS(v)   (((v) & 0xF) << 7)
#define NVME_CC_IOSQES(v) (((v) & 0xF) << 16)
#define NVME_CC_IOCQES(v) (((v) & 0xF) << 20)

/* CSTS bits */
#define NVME_CSTS_RDY    (1 << 0)
#define NVME_CSTS_CFS    (1 << 1)

/* AQA bits */
#define NVME_AQA_ASQS(v) ((v) & 0xFFF)
#define NVME_AQA_ACQS(v) (((v) & 0xFFF) << 16)

/* Admin commands */
#define NVME_ADM_IDENTIFY      0x06
#define NVME_ADM_CREATE_IOCQ   0x05
#define NVME_ADM_CREATE_IOSQ   0x01
#define NVME_ADM_SET_FEATURES  0x09

/* I/O commands */
#define NVME_IO_READ   0x02
#define NVME_IO_WRITE  0x01

/* Identify CNS values */
#define NVME_IDENTIFY_NS     0x00
#define NVME_IDENTIFY_CTRL   0x01

/* Queue types */
#define NVME_CQ 1
#define NVME_SQ 2

/* Submission Queue entry (64 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved1[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_cmd_t;

/* Completion Queue entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t cdw0;
    uint32_t reserved1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;  /* bits 15:14 = P, 13:12 = SCT, 7:0 = SC */
} nvme_cpl_t;

/* Identify Namespace data (4096 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t  nsfeat;
    uint8_t  nlbaf;
    uint8_t  flbas;
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t noiob;
    uint8_t  nvmcap[16];
    uint64_t reserved1[2];
    uint32_t anagrpid;
    uint32_t reserved2;
    uint8_t  nguid[16];
    uint8_t  eui64[8];
    uint8_t  lba_format[128];
    uint8_t  reserved3[3776];
} nvme_ns_data_t;

/* Identify Controller data (4096 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  reserved[512 - 78];
    uint32_t tnvmcap[2];
    uint32_t unvmcap[2];
    uint32_t rpmbs[4];
    /* much more... we only read the first 512 bytes */
} nvme_ctrl_data_t;

/* NVMe controller state */
typedef struct {
    int         present;
    volatile uint32_t* regs;    /* MMIO registers */
    uint64_t    regs_phys;
    int         doorbell_stride; /* in bytes */
    int         timeout_500ms;

    /* Admin queues */
    nvme_cmd_t*   asq;        /* Admin Submission Queue */
    uint64_t      asq_phys;
    nvme_cpl_t*   acq;        /* Admin Completion Queue */
    uint64_t      acq_phys;
    volatile int  acq_phase;  /* Phase tag for ACQ */

    /* I/O queues */
    nvme_cmd_t*   iosq;       /* I/O Submission Queue */
    uint64_t      iosq_phys;
    nvme_cpl_t*   iocq;       /* I/O Completion Queue */
    uint64_t      iocq_phys;
    volatile int  iocq_phase;

    int           q_depth;    /* Queue depth (entries) */
    uint16_t      next_cid;   /* Next command ID */

    uint64_t      nsze;
    uint32_t      lba_shift;  /* LBA size shift (9 = 512 bytes) */
    spinlock_t    lock;
} nvme_ctrl_t;

err_t nvme_init(void);

#endif
