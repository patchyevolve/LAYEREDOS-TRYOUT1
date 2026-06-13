#ifndef GPT_H
#define GPT_H

#include "types.h"
#include "block.h"

#define GPT_SIGNATURE 0x5452415020494645ULL  /* "EFI PART" */
#define GPT_MAX_PARTS 128
#define GPT_PART_NAME 72

typedef struct __attribute__((packed)) {
    uint8_t  boot_code[440];
    uint32_t disk_id;
    uint32_t reserved;
    struct {
        uint8_t  status;
        uint8_t  type[3];
        uint8_t  start_chs[3];
        uint32_t start_lba;
        uint32_t sector_count;
    } mbr_part[4];
    uint16_t signature;  /* 0x55AA for MBR */
} gpt_mbr_t;

typedef struct __attribute__((packed)) {
    uint64_t signature;           /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  guid[16];
    uint64_t entry_start_lba;
    uint32_t entry_count;
    uint32_t entry_size;          /* usually 128 */
    uint32_t entry_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint8_t  name[GPT_PART_NAME];  /* UTF-16LE */
} gpt_entry_t;

void gpt_scan(void);

#endif
