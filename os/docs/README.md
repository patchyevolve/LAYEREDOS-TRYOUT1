# OPERtur/TRY1 — Documentation Index

This directory mirrors the source tree under `os/src/` and contains one
Markdown file per source file.  Each doc explains **what the file does**,
**why it exists**, and **what every important piece means** — without
reproducing the source verbatim.

---

## Directory structure

```
docs/
├── README.md                       ← this file
├── Makefile.md                     ← build system
├── linker.md                       ← linker script
├── boot/
│   ├── boot.S.md                   ← multiboot entry, paging, long-mode jump
│   ├── isr.S.md                    ← interrupt stub table
│   ├── user_program.S.md           ← embedded ring-3 hello-world binary
│   └── cat_program.S.md            ← ring-3 cat utility (VFS open/read/write)
├── include/
│   ├── types.h.md                  ← primitive types, memory constants, err_t
│   ├── kernel.h.md                 ← top-level kernel API surface
│   ├── errno.h.md                  ← error-code string table
│   ├── kmalloc.h.md                ← heap allocator interface
│   ├── syscall.h.md                ← syscall dispatch interface
│   ├── syscall_defs.h.md           ← shared syscall number table (kernel + user)
│   └── process.h.md                ← process control block and process manager interface
├── kernel/
│   ├── ctx.S.md                    ← context switch & thread trampoline
│   ├── hal.c.md                    ← hardware abstraction layer
│   ├── hal.h.md                    ← HAL public interface
│   ├── pmm.c.md                    ← physical memory manager
│   ├── pmm.h.md                    ← PMM public interface
│   ├── vmm.c.md                    ← virtual memory / page-table walker
│   ├── vmm.h.md                    ← VMM public interface
│   ├── sched.c.md                  ← scheduler, threads, context lifecycle
│   ├── sched.h.md                  ← scheduler public interface
│   ├── sync.c.md                   ← spinlock, mutex, condvar
│   ├── sync.h.md                   ← sync primitives interface
│   ├── syscall.c.md                ← syscall handler implementations
│   ├── kmalloc.c.md                ← slab allocator implementation
│   ├── eventbus.c.md               ← publish/subscribe event bus
│   ├── eventbus.h.md               ← event bus interface
│   ├── watchdog.c.md               ← health monitor daemon
│   ├── watchdog.h.md               ← watchdog interface
│   ├── shell.c.md                  ← interactive kernel shell (all commands)
│   ├── shell.h.md                  ← shell interface
│   ├── main.c.md                   ← kernel entry point & boot sequence
│   ├── ata.c.md                    ← ATA/IDE PIO block device driver
│   ├── ata.h.md                    ← ATA driver interface
│   ├── block.c.md                  ← block device registry
│   ├── block.h.md                  ← block device abstraction interface
│   ├── elf.c.md                    ← ELF64 loader
│   ├── elf.h.md                    ← ELF64 types and loader interface
│   ├── keyboard.c.md               ← PS/2 keyboard driver
│   ├── keyboard.h.md               ← keyboard driver interface
│   ├── pci.c.md                    ← PCI bus enumerator
│   ├── pci.h.md                    ← PCI interface
│   ├── pipe.c.md                   ← anonymous pipe (IPC via VFS)
│   ├── pipe.h.md                   ← pipe interface
│   ├── process.c.md                ← process manager
│   ├── ramdisk.c.md                ← VFS-backed read-only ramdisk
│   ├── ramdisk.h.md                ← ramdisk interface
│   ├── ramdisk_blk.c.md            ← writable 1 MB block-device ramdisk
│   ├── ramdisk_blk.h.md            ← block ramdisk interface
│   ├── vfs.c.md                    ← virtual filesystem switch
│   ├── vfs.h.md                    ← VFS interface and types
│   ├── sfs.c.md                    ← Simple Filesystem (inode-based)
│   └── sfs.h.md                    ← SFS interface and on-disk structures
└── lib/
    ├── klib.c.md                   ← kernel standard library
    └── libuser/
        ├── crt0.S.md               ← user-space C runtime startup (_start)
        ├── user.c.md               ← user-space syscall wrappers (libc)
        └── user.h.md               ← libuser header (types + syscall helpers)
```

---

## Layer map (quick reference)

| Layer | Files |
|-------|-------|
| 0 — Hardware | (target machine) |
| 1 — HAL | `hal.c/h`, `boot.S`, `ata.c/h`, `keyboard.c/h`, `pci.c/h` |
| 2 — Scheduler | `sched.c/h`, `ctx.S`, `sync.c/h` |
| 3 — Process | `process.c`, `process.h`, `elf.c/h` |
| 4 — Memory | `pmm.c/h`, `vmm.c/h`, `kmalloc.c/h` |
| 5 — Filesystem | `vfs.c/h`, `sfs.c/h`, `ramdisk.c/h`, `ramdisk_blk.c/h` |
| N-1 — Shell/IPC | `shell.c/h`, `syscall.c/h`, `pipe.c/h` |
| Cross-cutting | `eventbus.c/h`, `watchdog.c/h` |
| Support | `klib.c`, `types.h`, `kernel.h`, `errno.h`, `syscall_defs.h` |
| User space | `crt0.S`, `user.c/h`, `user_program.S`, `cat_program.S` |
