/* ld.so — dynamic linker/loader for OPERtur/TRY1
 *
 * Entry: _start(argc, argv, envp).
 * The aux vector follows the NULL envp terminator on the stack.
 * After loading all shared libraries and resolving symbols,
 * we jump to the main program's entry (AT_ENTRY from auxv).
 */

/* ------------------------------------------------------------------ */
/*  Syscall wrappers (no libc dependency)                              */
/* ------------------------------------------------------------------ */
static long syscall3(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return r;
}
static long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return r;
}

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_READFILE 7
#define SYS_LSEEK   21
#define SYS_MMAP    31
#define SYS_MUNMAP  32
#define SYS_MPROTECT 33

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDOUT 1

#define PAGE_SIZE 4096

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

static void ld_write(const char* s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, STDOUT, (long)s, (unsigned long)len);
}


static void ld_exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  ELF / auxv types                                                  */
/* ------------------------------------------------------------------ */
#define ELF_MAGIC    0x464C457Fu
#define ELF_64       2
#define ELF_DYN      3

#define PT_NULL      0
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_PHDR      6
#define PF_X         1
#define PF_W         2
#define PF_R         4

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_INIT      12
#define DT_FINI      13
#define DT_PLTREL    20
#define DT_PLTRELSZ  2
#define DT_JMPREL    23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef uint64_t           size_t;

typedef struct {
    uint32_t  magic;
    uint8_t   cls, data, version, osabi, abiver;
    uint8_t   pad[7];
    uint16_t  type, machine;
    uint32_t  version2;
    uint64_t  entry, phoff, shoff;
    uint32_t  flags;
    uint16_t  ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf_hdr_t;

typedef struct {
    uint32_t  type, flags;
    uint64_t  offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) elf_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed)) elf_sym_t;

typedef struct {
    uint64_t r_offset, r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf_rela_t;

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((unsigned)(i))
#define ELF64_ST_TYPE(i)  ((i) & 0xf)
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define STB_GLOBAL   1
#define STB_WEAK     2
#define STT_FUNC     2
#define STT_OBJECT   1
#define STT_NOTYPE   0

/* ------------------------------------------------------------------ */
/*  Minimal utility helpers                                           */
/* ------------------------------------------------------------------ */
static void* memset(void* dst, int c, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return dst;
}
static void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/*  File operations via kernel syscalls                                */
/* ------------------------------------------------------------------ */
static int file_size(const char* path) {
    int fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) return -1;
    long sz = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
    syscall3(SYS_CLOSE, fd, 0, 0);
    return (int)sz;
}
static long file_read_all(const char* path, void* buf, long maxsz) {
    int fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) return -1;
    long sz = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
    if (sz > maxsz) sz = maxsz;
    syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    long total = 0;
    while (total < sz) {
        long chunk = sz - total;
        if (chunk > 512) chunk = 512;
        long n = syscall3(SYS_READFILE, fd, (long)((uint8_t*)buf + total), (unsigned long)chunk);
        if (n <= 0) break;
        if (n > chunk) n = chunk;
        total += n;
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* ------------------------------------------------------------------ */
/*  Loaded shared object tracking                                     */
/* ------------------------------------------------------------------ */
#define MAX_SO 32
static struct so_entry {
    uint64_t base;
    uint64_t dyn;
    uint64_t strtab, strsz;
    uint64_t symtab, syment;
    uint64_t rela, relasz, relaent;
    uint64_t pltrel, pltrelsz;
    uint64_t init, fini;
    uint64_t init_array, init_arraysz;
    uint64_t fini_array, fini_arraysz;
    char     name[64];
} so_list[MAX_SO];
static int so_count = 0;

/* ------------------------------------------------------------------ */
/*  Load a shared object from file                                     */
/* ------------------------------------------------------------------ */
static int load_so(const char* path) {
    for (int i = 0; i < so_count; i++)
        if (strcmp(so_list[i].name, path) == 0) return 0;

    if (so_count >= MAX_SO) { ld_write("ld.so: too many libs\n"); return -1; }

    int fsz = file_size(path);
    if (fsz < 0) { ld_write("ld.so: can't open "); ld_write(path); ld_write("\n"); return -1; }

    /* Allocate buffer via mmap (stack is only 4 KB) */
    unsigned char* file_buf = (unsigned char*)syscall6(SYS_MMAP, 0, fsz + 4096,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if ((long)file_buf < 0) { ld_write("ld.so: mmap buf fail\n"); return -1; }
    file_read_all(path, file_buf, fsz + 4096);

    elf_hdr_t* hdr = (elf_hdr_t*)file_buf;
    if (hdr->magic != ELF_MAGIC || hdr->cls != ELF_64) { ld_write("ld.so: bad ELF\n"); return -1; }
    if ((uint64_t)hdr->phoff > (uint64_t)fsz || (uint64_t)hdr->phoff < sizeof(elf_hdr_t)) { ld_write("ld.so: bad phoff\n"); return -1; }
    uint64_t ph_end = (uint64_t)hdr->phoff + (uint64_t)hdr->phnum * sizeof(elf_phdr_t);
    if (ph_end > (uint64_t)fsz) { ld_write("ld.so: phdrs past end\n"); return -1; }

    uint64_t load_base = 0x7E000000 - (uint64_t)(so_count + 1) * 0x200000;

    uint64_t last_end = 0;
    elf_phdr_t* ph = (elf_phdr_t*)(file_buf + hdr->phoff);
    for (unsigned i = 0; i < hdr->phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].vaddr > ~load_base || ph[i].memsz > ~(ph[i].vaddr + load_base)) { ld_write("ld.so: overflow\n"); return -1; }
            uint64_t map_at    = ph[i].vaddr + load_base;
            uint64_t map_end   = map_at + ph[i].memsz;
            if (map_end > last_end) last_end = map_end;

            uint64_t map_page  = map_at & ~(PAGE_SIZE - 1);
            uint64_t map_sz    = map_end - map_page;
            uint64_t page_len  = (map_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t page_skip = map_at - map_page;

            int prot = PROT_READ;
            if (ph[i].flags & PF_W) prot |= PROT_WRITE;
            if (ph[i].flags & PF_X) prot |= PROT_EXEC;

            long addr = syscall6(SYS_MMAP, (long)map_page, page_len,
                                 prot | PROT_WRITE,
                                 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, 0, 0);
            if (addr < 0) { ld_write("ld.so: mmap fail\n"); return -1; }

            memcpy((void*)(map_page + page_skip), file_buf + ph[i].offset,
                   ph[i].filesz < ph[i].memsz ? ph[i].filesz : ph[i].memsz);
            if (ph[i].memsz > ph[i].filesz)
                memset((void*)(map_page + page_skip + ph[i].filesz), 0,
                       ph[i].memsz - ph[i].filesz);

            if (!(prot & PROT_WRITE))
                syscall3(SYS_MPROTECT, (long)map_page, page_len, prot);
        }
    }

    struct so_entry* so = &so_list[so_count];
    int nlen = strlen(path);
    if ((unsigned)nlen >= sizeof(so->name)) nlen = sizeof(so->name) - 1;
    memcpy(so->name, path, nlen);
    so->name[nlen] = 0;
    so->base = load_base;
    so->dyn = 0;

    elf_phdr_t* ph2 = (elf_phdr_t*)(file_buf + hdr->phoff);
    uint64_t dyn_memsz = 0;
    for (unsigned i = 0; i < hdr->phnum; i++) {
        if (ph2[i].type == PT_DYNAMIC) { so->dyn = ph2[i].vaddr + load_base; dyn_memsz = ph2[i].memsz; break; }
    }

    if (so->dyn) {
        so->strtab = 0; so->symtab = 0; so->rela = 0; so->pltrel = 0;
        so->init = 0; so->fini = 0;
        so->init_array = 0; so->init_arraysz = 0;
        so->fini_array = 0; so->fini_arraysz = 0;
        uint64_t dyn_end = so->dyn + dyn_memsz;
        elf_dyn_t* dyn = (elf_dyn_t*)so->dyn;
        while (dyn->d_tag != DT_NULL && (uint64_t)(dyn + 1) <= dyn_end) {
            switch (dyn->d_tag) {
            case DT_STRTAB: so->strtab = dyn->d_val + load_base; break;
            case DT_STRSZ:  so->strsz  = dyn->d_val; break;
            case DT_SYMTAB: so->symtab = dyn->d_val + load_base; break;
            case DT_SYMENT: so->syment = dyn->d_val; break;
            case DT_RELA:   so->rela   = dyn->d_val + load_base; break;
            case DT_RELASZ: so->relasz = dyn->d_val; break;
            case DT_RELAENT:so->relaent= dyn->d_val; break;
            case DT_PLTREL: so->pltrel = dyn->d_val; break;
            case DT_PLTRELSZ: so->pltrelsz = dyn->d_val; break;
            case DT_JMPREL: so->pltrel = dyn->d_val + load_base; break;
            case DT_INIT:   so->init   = dyn->d_val + load_base; break;
            case DT_FINI:   so->fini   = dyn->d_val + load_base; break;
            case DT_INIT_ARRAY:   so->init_array   = dyn->d_val + load_base; break;
            case DT_INIT_ARRAYSZ: so->init_arraysz = dyn->d_val; break;
            case DT_FINI_ARRAY:   so->fini_array   = dyn->d_val + load_base; break;
            case DT_FINI_ARRAYSZ: so->fini_arraysz = dyn->d_val; break;
            case DT_NEEDED: {
                const char* dep = (const char*)(so->strtab + dyn->d_val);
                load_so(dep);
                break;
            }
            }
            dyn++;
        }
        if (dyn->d_tag != DT_NULL) { ld_write("ld.so: no DT_NULL\n"); return -1; }
    }

    so_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Symbol lookup                                                      */
/* ------------------------------------------------------------------ */
static uint64_t find_sym(const char* name) {
    for (int s = 0; s < so_count; s++) {
        struct so_entry* so = &so_list[s];
        if (!so->symtab || !so->strtab) continue;
        unsigned nsym = so->strsz / (so->syment ? so->syment : sizeof(elf_sym_t));
        elf_sym_t* symtab = (elf_sym_t*)so->symtab;
        for (unsigned i = 0; i < nsym; i++) {
            elf_sym_t* sym = &symtab[i];
            if (sym->st_name >= so->strsz) continue;
            unsigned bind = ELF64_ST_BIND(sym->st_info);
            if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
                sym->st_name != 0 && sym->st_value != 0) {
                const char* sym_name = (const char*)(so->strtab + sym->st_name);
                if (strcmp(sym_name, name) == 0)
                    return so->base + sym->st_value;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Apply relocations                                                  */
/* ------------------------------------------------------------------ */
static void process_rela(struct so_entry* so) {
    if (so->rela && so->relasz) {
        unsigned long relaent = so->relaent ? so->relaent : 24;
        unsigned long n = so->relasz / relaent;
        elf_rela_t* r = (elf_rela_t*)so->rela;
        unsigned nsym_so = so->strsz / (so->syment ? so->syment : sizeof(elf_sym_t));
        for (unsigned long i = 0; i < n; i++) {
            if (r[i].r_offset > ~so->base) continue;
            uint64_t* addr = (uint64_t*)(so->base + r[i].r_offset);
            unsigned type = ELF64_R_TYPE(r[i].r_info);
            unsigned sym_idx = ELF64_R_SYM(r[i].r_info);
            switch (type) {
            case R_X86_64_RELATIVE:
                *addr = so->base + r[i].r_addend;
                break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
            case R_X86_64_64:
                if (sym_idx >= nsym_so) continue;
                if (sym_idx) {
                    elf_sym_t* sym = &((elf_sym_t*)so->symtab)[sym_idx];
                    if (sym->st_name >= so->strsz) continue;
                    const char* sym_name = (const char*)(so->strtab + sym->st_name);
                    uint64_t val = find_sym(sym_name);
                    if (!val) { ld_write("ld.so: unresolved: "); ld_write(sym_name); ld_write("\n"); continue; }
                    if (type == R_X86_64_64)
                        *addr = val + r[i].r_addend;
                    else
                        *addr = val;
                }
                break;
            case R_X86_64_PC32:
                if (sym_idx >= nsym_so) continue;
                if (sym_idx) {
                    elf_sym_t* sym = &((elf_sym_t*)so->symtab)[sym_idx];
                    if (sym->st_name >= so->strsz) continue;
                    const char* sym_name = (const char*)(so->strtab + sym->st_name);
                    uint64_t val = find_sym(sym_name);
                    if (!val) { ld_write("ld.so: unresolved: "); ld_write(sym_name); ld_write("\n"); continue; }
                    *(uint32_t*)addr = (uint32_t)(val + r[i].r_addend - (uint64_t)addr);
                }
                break;
            }
        }
    }

    if (so->pltrel && so->pltrelsz) {
        unsigned long n = so->pltrelsz / 24;
        elf_rela_t* r = (elf_rela_t*)so->pltrel;
        unsigned nsym_so = so->strsz / (so->syment ? so->syment : sizeof(elf_sym_t));
        for (unsigned long i = 0; i < n; i++) {
            uint64_t* addr = (uint64_t*)(so->base + r[i].r_offset);
            unsigned type = ELF64_R_TYPE(r[i].r_info);
            unsigned sym_idx = ELF64_R_SYM(r[i].r_info);
            if (type == R_X86_64_JUMP_SLOT && sym_idx) {
                if (sym_idx >= nsym_so) continue;
                elf_sym_t* sym = &((elf_sym_t*)so->symtab)[sym_idx];
                if (sym->st_name >= so->strsz) continue;
                const char* sym_name = (const char*)(so->strtab + sym->st_name);
                uint64_t val = find_sym(sym_name);
                if (!val) { ld_write("ld.so: unresolved PLT: "); ld_write(sym_name); ld_write("\n"); continue; }
                *addr = val;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  _start — main entry point                                         */
/* ------------------------------------------------------------------ */
void __attribute__((noinline)) _start(void) {
    /* Read argc/argv[0] and auxv from fixed addresses written by the kernel. */
    unsigned long argc;
    unsigned long argv0;
    uint64_t* auxv;
    asm volatile("movq 0x7FFF00F0, %0\n\t"
                 "movq 0x7FFF00F8, %1\n\t"
                 "movq $0x7FFF0100, %2"
                 : "=r"(argc), "=r"(argv0), "=r"(auxv));

    uint64_t at_entry = 0, at_phdr = 0, at_phnum = 0;
    for (uint64_t* ap = auxv; ap[0] != 0; ap += 2) {
        switch (ap[0]) {
        case 3:  at_phdr  = ap[1]; break;
        case 5:  at_phnum = ap[1]; break;
        case 9:  at_entry = ap[1]; break;
        }
    }

    if (!at_entry) { ld_write("ld.so: no AT_ENTRY\n"); ld_exit(1); }

    /* Add the main executable as the first SO for symbol resolution */
    if (at_phdr && at_phnum) {
        struct so_entry* main_so = &so_list[so_count];
        memset(main_so, 0, sizeof(*main_so));
        /* For ET_EXEC (non-PIE) base=0; for ET_DYN (PIE) base is implicit in vaddrs.
         * We determine base by checking if the ELF header at at_entry has type.
         * For now assume ET_EXEC with base=0. */
        main_so->base = 0;
        memcpy(main_so->name, "main", 5);
        /* Scan program headers to find PT_DYNAMIC */
        elf_phdr_t* ph = (elf_phdr_t*)at_phdr;
        for (unsigned i = 0; i < (unsigned)at_phnum; i++) {
            if (ph[i].type == PT_DYNAMIC)
                { main_so->dyn = ph[i].vaddr; break; }  /* vaddr already includes base */
        }
        if (main_so->dyn) {
            elf_dyn_t* dyn = (elf_dyn_t*)main_so->dyn;
            while (dyn->d_tag != DT_NULL) {
                switch (dyn->d_tag) {
                case DT_STRTAB: main_so->strtab = dyn->d_val; break;
                case DT_STRSZ:  main_so->strsz  = dyn->d_val; break;
                case DT_SYMTAB: main_so->symtab = dyn->d_val; break;
                case DT_SYMENT: main_so->syment = dyn->d_val; break;
                case DT_RELA:   main_so->rela   = dyn->d_val; break;
                case DT_RELASZ: main_so->relasz = dyn->d_val; break;
                case DT_RELAENT:main_so->relaent= dyn->d_val; break;
                case DT_PLTREL: main_so->pltrel = dyn->d_val; break;
                case DT_PLTRELSZ: main_so->pltrelsz = dyn->d_val; break;
                case DT_JMPREL: main_so->pltrel = dyn->d_val; break;
                }
                dyn++;
            }
        }
        so_count++;
    }

    /* Load libdyn.so */
    if (load_so("/libdyn.so") != 0) {
        ld_write("ld.so: failed to load libdyn.so\n");
        ld_exit(1);
    }

    /* Apply relocations for all loaded shared objects */
    for (int i = 0; i < so_count; i++)
        process_rela(&so_list[i]);

    /* Call init functions */
    for (int i = 0; i < so_count; i++) {
        if (so_list[i].init) {
            void (*init_fn)(void) = (void (*)(void))so_list[i].init;
            init_fn();
        }
        if (so_list[i].init_array && so_list[i].init_arraysz) {
            unsigned n = so_list[i].init_arraysz / 8;
            void (**init_arr)(void) = (void (**)(void))so_list[i].init_array;
            for (unsigned j = 0; j < n; j++)
                if (init_arr[j]) init_arr[j]();
        }
    }

    /* Build argv on stack — we only have argv[0] */
    if (argc > 1) argc = 1;
    unsigned long argv_data[2];
    argv_data[0] = argv0;
    argv_data[1] = 0;

    /* Jump to main program entry: entry(argc, argv, envp) */
    typedef void (*entry_t)(unsigned long, unsigned long, unsigned long);
    entry_t entry = (entry_t)at_entry;
    entry(argc, (unsigned long)argv_data, 0);

    ld_exit(0);
}
