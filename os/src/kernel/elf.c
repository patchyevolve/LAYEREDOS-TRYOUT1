#include "kernel.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "hal.h"

static inline uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* Simple ASLR: generate a page-aligned random offset */
static uint64_t aslr_offset(uint64_t max_pages) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t rdtsc_val = ((uint64_t)hi << 32) | lo;
    uint64_t ticks = hal_timer_get_ticks();
    uint64_t stack_addr = (uint64_t)&lo;
    uint64_t seed = rdtsc_val ^ (ticks * 6364136223846793005ULL + 1442695040888963407ULL);
    seed ^= stack_addr << 12;
    uint64_t pages;
    if (max_pages & (max_pages - 1))
        pages = seed % max_pages;
    else
        pages = seed & (max_pages - 1);
    return pages * PAGE_SIZE;
}

static err_t elf_map_segment(process_t* proc, const elf64_phdr_t* ph,
                             uint64_t cr3, const void* elf_base,
                             uint64_t base_offset) {
    uint64_t vaddr  = ph->vaddr + base_offset;
    uint64_t filesz = ph->filesz;
    uint64_t memsz  = ph->memsz;
    uint64_t offset = ph->offset;
    uint64_t pgfl   = PAGE_USER;

    if (ph->flags & PF_W) pgfl |= PAGE_WRITE;

    if (memsz > ~vaddr) return ERR_INVAL;
    uint64_t seg_start = vaddr;
    uint64_t seg_end   = vaddr + memsz;
    uint64_t file_start = offset;

    uint64_t first_page = seg_start & PAGE_MASK;
    uint64_t last_page  = ((seg_end + PAGE_SIZE - 1) & PAGE_MASK);

    for (uint64_t page = first_page; page < last_page; page += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return ERR_NOMEM;

        kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);

        uint64_t overlap_start = max_u64(page, seg_start);
        uint64_t overlap_end   = min_u64(page + PAGE_SIZE, seg_start + filesz);

        if (overlap_start < overlap_end) {
            uint64_t page_off = overlap_start - page;
            uint64_t file_off = overlap_start - seg_start + file_start;
            uint64_t len      = overlap_end - overlap_start;

            kmemcpy((void*)(PHYS_TO_VIRT(phys) + page_off),
                    (const void*)((uint64_t)elf_base + file_off), len);
        }

        err_t e = vmm_map_page(cr3, page, phys, pgfl);
        if (e) {
            pmm_free_page(phys);
            return e;
        }
    }
    return ERR_OK;
}

err_t elf_load(process_t* proc, const void* elf_data, size_t elf_len) {
    if (!elf_data || elf_len < sizeof(elf64_hdr_t))
        return ERR_INVAL;

    const elf64_hdr_t* hdr = (const elf64_hdr_t*)elf_data;

    if (hdr->magic != ELF_MAGIC || hdr->cls != ELF_64)
        return ERR_INVAL;

    /* Accept both EXEC (non-PIE) and DYN (PIE) */
    if (hdr->type != ELF_EXEC && hdr->type != ELF_DYN)
        return ERR_INVAL;

    if ((uint64_t)hdr->phnum > ~0ULL / (uint64_t)hdr->phentsize) return ERR_INVAL;
    if (hdr->phoff + (uint64_t)hdr->phnum * hdr->phentsize > elf_len)
        return ERR_INVAL;

    /* For PIE, compute a random base offset within user space */
    uint64_t base_offset = 0;
    if (hdr->type == ELF_DYN) {
        /* Range: [0x40000000, 0x60000000) in page increments */
        base_offset = 0x40000000 + aslr_offset(0x20000); /* 0x20000 pages = 512 MiB */
    }

    const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + hdr->phoff);
    for (uint16_t i = 0; i < hdr->phnum; i++) {
        if (ph->type == PT_LOAD) {
            err_t e = elf_map_segment(proc, ph, proc->cr3, elf_data, base_offset);
            if (e) return e;
        }
        ph = (const elf64_phdr_t*)((uint8_t*)ph + hdr->phentsize);
    }

    proc->entry_point = hdr->entry + base_offset;
    return ERR_OK;
}

err_t elf_load_fixed(process_t* proc, const void* elf_data, size_t elf_len, uint64_t base) {
    if (!elf_data || elf_len < sizeof(elf64_hdr_t))
        return ERR_INVAL;

    const elf64_hdr_t* hdr = (const elf64_hdr_t*)elf_data;

    if (hdr->magic != ELF_MAGIC || hdr->cls != ELF_64)
        return ERR_INVAL;

    if (hdr->type != ELF_EXEC && hdr->type != ELF_DYN)
        return ERR_INVAL;

    if ((uint64_t)hdr->phnum > ~0ULL / (uint64_t)hdr->phentsize) return ERR_INVAL;
    if (hdr->phoff + (uint64_t)hdr->phnum * hdr->phentsize > elf_len)
        return ERR_INVAL;

    const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + hdr->phoff);
    for (uint16_t i = 0; i < hdr->phnum; i++) {
        if (ph->type == PT_LOAD) {
            err_t e = elf_map_segment(proc, ph, proc->cr3, elf_data, base);
            if (e) return e;
        }
        ph = (const elf64_phdr_t*)((uint8_t*)ph + hdr->phentsize);
    }

    proc->entry_point = hdr->entry + base;
    return ERR_OK;
}

uint64_t elf_phdr_vaddr(const void* elf_data, uint64_t base) {
    const elf64_hdr_t* hdr = (const elf64_hdr_t*)elf_data;
    if (hdr->magic != ELF_MAGIC) return 0;

    const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + hdr->phoff);
    for (uint16_t i = 0; i < hdr->phnum; i++) {
        if (ph->type == PT_LOAD) {
            uint64_t seg_start = ph->vaddr + base;
            if (hdr->phoff >= ph->offset && hdr->phoff < ph->offset + ph->filesz) {
                return seg_start + (hdr->phoff - ph->offset);
            }
        }
        ph = (const elf64_phdr_t*)((uint8_t*)ph + hdr->phentsize);
    }
    /* Fallback: assume headers at base + phoff */
    return base + hdr->phoff;
}
