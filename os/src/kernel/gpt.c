#include "gpt.h"
#include "block.h"
#include "kmalloc.h"
#include "kernel.h"

#define MAX_BLOCK_DEVICES 8

/* Per-partition private data for block_dev_t */
typedef struct {
    block_dev_t* parent;
    uint64_t     lba_offset;
} part_priv_t;

static err_t part_read(block_dev_t* dev, uint64_t lba, uint8_t count, void* buf) {
    part_priv_t* p = (part_priv_t*)dev->private_data;
    return p->parent->read(p->parent, lba + p->lba_offset, count, buf);
}

static err_t part_write(block_dev_t* dev, uint64_t lba, uint8_t count, const void* buf) {
    part_priv_t* p = (part_priv_t*)dev->private_data;
    return p->parent->write(p->parent, lba + p->lba_offset, count, buf);
}

static void format_guid_part_name(const gpt_entry_t* e, char* out, int out_sz) {
    /* Convert name from UTF-16LE to ASCII — skip non-ASCII chars */
    int oi = 0;
    for (int i = 0; i < GPT_PART_NAME && oi < out_sz - 1; i += 2) {
        uint16_t c = (uint16_t)e->name[i] | ((uint16_t)e->name[i + 1] << 8);
        if (c >= 0x20 && c <= 0x7E)
            out[oi++] = (char)c;
        else if (c == 0)
            break;
    }
    out[oi] = 0;
}

/* Strip trailing whitespace from partition name */
static void strip_trailing(char* s) {
    int len = kstrlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = 0;
}

void gpt_scan(void) {
    int bcnt = block_count();
    for (int bi = 0; bi < bcnt; bi++) {
        block_dev_t* parent = block_get(bi);
        if (!parent) continue;

        /* Read protective MBR (LBA 0) */
        uint8_t* mbr_buf = kmalloc(BLOCK_SIZE);
        if (!mbr_buf) continue;
        if (parent->read(parent, 0, 1, mbr_buf) != ERR_OK) {
            kfree(mbr_buf);
            continue;
        }
        gpt_mbr_t* mbr = (gpt_mbr_t*)mbr_buf;

        /* Check MBR signature and partition type 0xEE (protective) */
        int has_gpt = 0;
        if (mbr->signature == 0xAA55) {
            for (int j = 0; j < 4; j++) {
                if (mbr->mbr_part[j].type[0] == 0xEE &&
                    mbr->mbr_part[j].type[1] == 0 &&
                    mbr->mbr_part[j].type[2] == 0) {
                    has_gpt = 1;
                    break;
                }
            }
        }
        kfree(mbr_buf);
        if (!has_gpt) continue;

        /* Read GPT header (LBA 1) */
        gpt_header_t* hdr = (gpt_header_t*)kmalloc(BLOCK_SIZE);
        if (!hdr) continue;
        if (parent->read(parent, 1, 1, hdr) != ERR_OK ||
            hdr->signature != GPT_SIGNATURE ||
            hdr->entry_size < sizeof(gpt_entry_t) ||
            hdr->entry_count == 0 ||
            hdr->entry_start_lba == 0) {
            kfree(hdr);
            continue;
        }

        uint32_t entry_cnt = hdr->entry_count;
        if (entry_cnt > GPT_MAX_PARTS) entry_cnt = GPT_MAX_PARTS;
        uint32_t entry_blk_sz = (entry_cnt * hdr->entry_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

        /* Read partition entries */
        uint8_t* entries = kmalloc(entry_blk_sz * BLOCK_SIZE);
        if (!entries) { kfree(hdr); continue; }
        if (parent->read(parent, hdr->entry_start_lba, (uint8_t)entry_blk_sz, entries) != ERR_OK) {
            kfree(entries);
            kfree(hdr);
            continue;
        }

        /* Register each partition */
        int part_count = 0;
        for (uint32_t pi = 0; pi < entry_cnt; pi++) {
            gpt_entry_t* e = (gpt_entry_t*)(entries + pi * hdr->entry_size);
            /* Skip empty partitions (zero GUID) */
            int empty = 1;
            for (int gi = 0; gi < 16; gi++) {
                if (e->type_guid[gi] != 0) { empty = 0; break; }
            }
            if (empty) continue;

            uint64_t blocks = e->end_lba - e->start_lba + 1;
            if (blocks == 0 || e->start_lba == 0) continue;

            /* Build partition name: parent name + "p" + index */
            char pname[20];
            char pname_utf8[GPT_PART_NAME];
            format_guid_part_name(e, pname_utf8, sizeof(pname_utf8));
            strip_trailing(pname_utf8);
            /* sprintf parent name + p%d manually */
            int plen = kstrlen(parent->name);
            if (plen > 14) plen = 14;
            kmemcpy(pname, parent->name, plen);
            pname[plen] = 'p';
            int part_num = pi + 1;
            int d = 100;
            int started = 0;
            int oi = plen + 1;
            while (d > 0) {
                int digit = part_num / d;
                if (digit || started || d == 1) {
                    pname[oi++] = '0' + digit;
                    started = 1;
                }
                part_num %= d;
                d /= 10;
            }
            pname[oi] = 0;

            part_priv_t* priv = kmalloc(sizeof(part_priv_t));
            if (!priv) continue;
            priv->parent = parent;
            priv->lba_offset = e->start_lba;

            block_dev_t pdev;
            kmemcpy(pdev.name, pname, sizeof(pdev.name));
            pdev.name[sizeof(pdev.name) - 1] = 0;
            pdev.block_count = blocks;
            pdev.block_size = BLOCK_SIZE;
            pdev.read = part_read;
            pdev.write = part_write;
            pdev.private_data = priv;

            int reg_idx = block_register(&pdev);
            if (reg_idx < 0) {
                kfree(priv);
                continue;
            }
            part_count++;
            kprintf("[GPT] Partition %s: LBA %llu-%llu (%llu blocks) \"%s\"\n",
                    pname, e->start_lba, e->end_lba, blocks, pname_utf8);
        }

        kprintf("[GPT] Device %s: %d partition(s) found\n", parent->name, part_count);
        kfree(entries);
        kfree(hdr);
    }
}
