#include "kernel.h"
#include "syscall.h"
#include "syscall_defs.h"
#include "sched.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "vfs.h"
#include "elf.h"
#include "kmalloc.h"
#include "pipe.h"
#include "tty.h"
#include "pty.h"
#include "net.h"
#include "ntp.h"
#include "errno.h"
#include "security.h"
#include "random.h"
#include "sha256.h"
#include "vma.h"
#include "net_ns.h"

/* Syscall-local constants */
#define CLONE_NEWNET  0x40000000
#include "unix.h"
#include "secure_boot.h"
#include "net_ns.h"
#include "veth.h"
#include "netconfig.h"
#include "block.h"
#include "arp.h"
#include "ndp.h"
#include "ipv4.h"
#include "route.h"

#define USER_VIRT_START 0x40000000UL
#define USER_VIRT_END   0x80000000UL

/* Translate kernel ERR_* codes to POSIX errno values for userspace.
 * Called at the syscall return boundary so all userspace syscall
 * wrappers (which do `errno = (int)(-ret)`) get the correct errno. */
static int kernel_err_to_posix(int kerr) {
    switch (kerr) {
    case ERR_GENERAL:   return EIO;
    case ERR_NOMEM:     return ENOMEM;
    case ERR_INVAL:     return EINVAL;
    case ERR_BADADDR:   return EFAULT;
    case ERR_BUSY:      return EBUSY;
    case ERR_TIMEOUT:   return ETIMEDOUT;
    case ERR_AGAIN:     return EAGAIN;
    case ERR_FAULT:     return EFAULT;
    case ERR_NOSYS:     return ENOSYS;
    case ERR_PERM:      return EPERM;
    case ERR_EXIST:     return EEXIST;
    case ERR_NOENT:     return ENOENT;
    case ERR_IO:        return EIO;
    case ERR_NOSPACE:   return ENOSPC;
    case ERR_NAMETOOLONG: return ENAMETOOLONG;
    case ERR_NOTCONN:   return ENOTCONN;
    case ERR_CONNREFUSED: return ECONNREFUSED;
    case ERR_BADFD:     return EBADF;
    default:            return EINVAL;
    }
}

static inline void smap_enable(void) {
    if (hal_smap_enabled())
        asm volatile("stac" ::: "memory");
}

static inline void smap_disable(void) {
    if (hal_smap_enabled())
        asm volatile("clac" ::: "memory");
}

static int is_user_range_valid(uint64_t addr, size_t len) {
    if (len == 0) return 0;
    uint64_t end = addr + len;
    if (end <= addr) return 0;
    if (addr < USER_VIRT_START) return 0;
    if (end > USER_VIRT_END) return 0;
    return 1;
}

static int is_user_memory_mapped(uint64_t addr, size_t len, int write) {
    uint64_t start_page = addr & ~0xFFFULL;
    uint64_t end_page = (addr + len + 0xFFFULL) & ~0xFFFULL;
    uint64_t cr3 = current_thread && current_thread->cr3
                   ? current_thread->cr3
                   : vmm_get_kernel_pml4();
    for (uint64_t page = start_page; page < end_page; page += PAGE_SIZE) {
        page_entry_t* pte = vmm_walk_pagetable(cr3, page);
        if (!pte) return 0;
        if (!(*pte & PAGE_PRESENT)) return 0;
        if (!(*pte & PAGE_USER)) return 0;
        if (write && !(*pte & PAGE_WRITE)) return 0;
    }
    return 1;
}

int copy_from_user(void* dst, const void* src, size_t n) {
    if (!dst || !src) return ERR_FAULT;
    if (n == 0) return 0;
    if (!is_user_range_valid((uint64_t)src, n)) return ERR_FAULT;
    if (!is_user_memory_mapped((uint64_t)src, n, 0)) return ERR_FAULT;

    cpu_flags_t flags = hal_save_irq();
    smap_enable();
    kmemcpy(dst, src, n);
    smap_disable();
    hal_restore_irq(flags);
    return 0;
}

int copy_to_user(void* dst, const void* src, size_t n) {
    if (!dst || !src) return ERR_FAULT;
    if (n == 0) return 0;
    if (!is_user_range_valid((uint64_t)dst, n)) return ERR_FAULT;
    if (!is_user_memory_mapped((uint64_t)dst, n, 1)) return ERR_FAULT;

    cpu_flags_t flags = hal_save_irq();
    smap_enable();
    kmemcpy(dst, src, n);
    smap_disable();
    hal_restore_irq(flags);
    return 0;
}

int strncpy_from_user(void* dst, const void* src, size_t max) {
    if (!dst || !src || max == 0) return ERR_FAULT;
    if (!is_user_range_valid((uint64_t)src, 1)) return ERR_FAULT;
    if (!is_user_range_valid((uint64_t)src + max - 1, 1)) return ERR_FAULT;

    cpu_flags_t flags = hal_save_irq();
    smap_enable();
    size_t i;
    for (i = 0; i < max; i++) {
        char c = ((volatile const char*)src)[i];
        ((volatile char*)dst)[i] = c;
        if (c == '\0') break;
    }
    smap_disable();
    hal_restore_irq(flags);
    if (i == max) return ERR_NAMETOOLONG;
    return (int)i;
}

static uint64_t sys_exit(int_frame_t* frame) {
    int exit_code = (int)frame->rdi;
    if (current_thread && current_thread->proc) {
        process_t* proc = current_thread->proc;
        unsigned long old = __sync_fetch_and_sub(&proc->thread_count, 1);
        if (old == 1) {
            /* Last thread — clean up the process */
            proc->exit_code = exit_code;
            proc->exited = true;
            process_exit(proc, exit_code);
        }
    }
    thread_exit(exit_code);
    return 0;
}

static uint64_t sys_write(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const char* buf = (const char*)frame->rsi;
    size_t count = (size_t)frame->rdx;

    /* Check if this is a socket fd */
    socket_t* s = sock_lookup(fd);
    if (s) {
        char kbuf[512];
        size_t written = 0;
        while (count > 0) {
            size_t chunk = count > sizeof(kbuf) ? sizeof(kbuf) : count;
            if (copy_from_user(kbuf, buf + written, chunk) != 0)
                return (uint64_t)(int64_t)ERR_FAULT;
            int ret = sock_send(s, (const uint8_t*)kbuf, chunk);
            if (ret < 0) return written ? (uint64_t)written : (uint64_t)(int64_t)ret;
            written += (size_t)ret;
            count -= (size_t)ret;
        }
        return (uint64_t)written;
    }

    char kbuf[512];
    size_t written = 0;
    while (count > 0) {
        size_t chunk = count;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, buf + written, chunk) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        int64_t ret = vfs_write(fd, kbuf, chunk);
        if (ret < 0) {
            /* If write to fd 0/1/2 and no fd is open, fall through to UART */
            if (fd >= 0 && fd <= 2) {
                for (size_t i = 0; i < (written ? written : chunk); i++)
                    kputchar(kbuf[i]);
                return (uint64_t)(written ? written : chunk);
            }
            return (uint64_t)(int64_t)ERR_IO;
        }
        written += (size_t)ret;
        count -= (size_t)ret;
    }
    return (uint64_t)written;
}

static uint64_t sys_read(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    char* buf = (char*)frame->rsi;
    size_t count = (size_t)frame->rdx;

    /* Check if this is a socket fd */
    socket_t* s = sock_lookup(fd);
    if (s) {
        char kbuf[512];
        size_t read_size = count > sizeof(kbuf) ? sizeof(kbuf) : count;
        int ret = sock_recv(s, (uint8_t*)kbuf, read_size);
        if (ret < 0) return (uint64_t)(int64_t)ret;
        if (copy_to_user(buf, kbuf, (size_t)ret) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        return (uint64_t)ret;
    }

    char kbuf[512];
    size_t total = 0;
    while (count > 0) {
        size_t chunk = count;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int64_t ret = vfs_read(fd, kbuf, chunk);
        if (ret < 0) return total ? (uint64_t)total : (uint64_t)(int64_t)ERR_IO;
        if (copy_to_user(buf + total, kbuf, (size_t)ret) != 0)
            return total ? (uint64_t)total : (uint64_t)(int64_t)ERR_FAULT;
        total += (size_t)ret;
        count -= (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (uint64_t)total;
}

static uint64_t sys_getpid(int_frame_t* frame) {
    (void)frame;
    if (current_thread && current_thread->proc)
        return current_thread->proc->pid;
    return 0;
}

static uint64_t sys_sbrk(int_frame_t* frame) {
    intptr_t increment = (intptr_t)frame->rdi;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)-1;

    uint64_t old_brk = proc->user_stack_top;
    if (increment > 0) {
        uint64_t new_brk = old_brk + (uint64_t)increment;
        uint64_t start_page = old_brk & PAGE_MASK;
        uint64_t end_page   = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
        uint64_t cr3_val = proc->cr3;
        for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
            page_entry_t* pte = vmm_walk_pagetable(cr3_val, addr);
            if (pte && (*pte & PAGE_PRESENT)) continue;
            uint64_t phys = pmm_alloc_page();
            if (!phys) return (uint64_t)-1;
            kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            vmm_map_page(cr3_val, addr, phys, PAGE_USER | PAGE_WRITE);
        }
        proc->user_stack_top = new_brk;
    }
    return old_brk;
}

static int copy_path_from_user(const char* user_path, char* kernel_buf, size_t max_len) {
    int ret = strncpy_from_user(kernel_buf, user_path, max_len);
    if (ret < 0) return ret;
    kernel_buf[max_len - 1] = '\0';
    return 0;
}

static uint64_t sys_open(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    int flags = (int)frame->rsi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    int fd = vfs_open(kernel_path, flags);
    if (fd < 0) return (uint64_t)(int64_t)ERR_NOENT;
    return (uint64_t)(uint32_t)fd;
}

static uint64_t sys_close(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    socket_t* s = sock_lookup(fd);
    if (s) {
        KDEBUG("[SYSCALL] close(fd=%d) socket\n", fd);
        sock_unregister(fd);
        sock_close(s);
        return 0;
    }
    if (vfs_close(fd) < 0)
        return (uint64_t)(int64_t)ERR_BADFD;
    return 0;
}

static uint64_t sys_readfile(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    char* buf = (char*)frame->rsi;
    size_t count = (size_t)frame->rdx;
    char kbuf[512];
    size_t total = 0;
    while (count > 0) {
        size_t chunk = count;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int64_t ret = vfs_read(fd, kbuf, chunk);
        if (ret < 0) return total ? (uint64_t)total : (uint64_t)(int64_t)ERR_IO;
        if (copy_to_user(buf + total, kbuf, (size_t)ret) != 0)
            return total ? (uint64_t)total : (uint64_t)(int64_t)ERR_FAULT;
        total += (size_t)ret;
        count -= (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (uint64_t)total;
}

static uint64_t sys_writefile(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const char* buf = (const char*)frame->rsi;
    size_t count = (size_t)frame->rdx;
    char kbuf[512];
    size_t written = 0;
    while (count > 0) {
        size_t chunk = count;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, buf + written, chunk) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        int64_t ret = vfs_write(fd, kbuf, chunk);
        if (ret < 0) return (uint64_t)(int64_t)ERR_IO;
        written += (size_t)ret;
        count -= (size_t)ret;
    }
    return (uint64_t)written;
}

static uint64_t sys_execve(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    int fd = vfs_open(kernel_path, 0);
    if (fd < 0) return (uint64_t)(int64_t)ERR_NOENT;
    uint64_t sz = (uint64_t)vfs_lseek(fd, 0, VFS_SEEK_END);
    vfs_lseek(fd, 0, VFS_SEEK_SET);
    if (sz == 0 || sz > 1024 * 1024) { vfs_close(fd); return (uint64_t)(int64_t)ERR_INVAL; }
    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if (!buf) { vfs_close(fd); return (uint64_t)(int64_t)ERR_NOMEM; }
    int64_t total = 0;
    while ((uint64_t)total < sz) {
        int64_t r = vfs_read(fd, buf + total, sz - (uint64_t)total);
        if (r <= 0) break;
        total += r;
    }
    vfs_close(fd);
    if ((uint64_t)total < sz) { kfree(buf); return (uint64_t)(int64_t)ERR_IO; }
    if (proc->cr3) {
        vmm_free_user_pages(proc->cr3);
        uint64_t new_cr3 = pmm_alloc_page();
        if (!new_cr3) { kfree(buf); return (uint64_t)(int64_t)ERR_NOMEM; }
        kmemset((void*)PHYS_TO_VIRT(new_cr3), 0, PAGE_SIZE);
        uint64_t* old_pml4 = (uint64_t*)PHYS_TO_VIRT(proc->cr3);
        uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(new_cr3);
        for (int i = 256; i < 512; i++) new_pml4[i] = old_pml4[i];
        pmm_free_page(proc->cr3);
        proc->cr3 = new_cr3;
    }
    vma_cleanup(proc);
    current_thread->cr3 = proc->cr3;
    asm volatile("mov %0, %%cr3" : : "r" (proc->cr3) : "memory");
    err_t e = elf_load(proc, buf, sz);
    kfree(buf);
    if (e) return (uint64_t)(int64_t)e;

    /* Check for setuid bit on the executable (unless no_new_privs) */
    if (!proc->no_new_privs) {
        vfs_stat_t ex_st;
        if (vfs_stat(kernel_path, &ex_st) == 0) {
            if (ex_st.mode & S_ISUID) {
                proc->euid = ex_st.uid;
            }
        }
    }

    uint64_t stack_page = pmm_alloc_page();
    if (!stack_page) return (uint64_t)(int64_t)ERR_NOMEM;
    kmemset((void*)PHYS_TO_VIRT(stack_page), 0, PAGE_SIZE);
    uint64_t user_stack = 0x70000000 + (aslr_rand() & 0xFF) * PAGE_SIZE;
    vmm_map_page(proc->cr3, user_stack, stack_page, PAGE_USER | PAGE_WRITE);

    uint8_t* stk = (uint8_t*)PHYS_TO_VIRT(stack_page);
    uint64_t off = PAGE_SIZE;
    size_t nlen = kstrlen(proc->name) + 1;
    off -= nlen; kmemcpy(stk + off, proc->name, nlen);
    off &= ~7ULL;
    uint64_t prog_vaddr = user_stack + off;
    off -= 8; *(uint64_t*)(stk + off) = 0;
    off -= 8; *(uint64_t*)(stk + off) = 0;
    off -= 8; *(uint64_t*)(stk + off) = 0;
    off -= 8; *(uint64_t*)(stk + off) = prog_vaddr;
    off -= 8; *(uint64_t*)(stk + off) = 1;

    frame->rip = proc->entry_point;
    frame->rsp = user_stack + off;
    frame->rdi = 1;
    frame->rsi = user_stack + off + 8;
    return 0;
}

static uint64_t sys_fork(int_frame_t* frame) {
    thread_t* parent = current_thread;
    process_t* pp = parent ? parent->proc : NULL;
    if (!pp) return (uint64_t)(int64_t)ERR_INVAL;
    /* Fork limit check */
    if (pp->fork_limit >= 0 && pp->fork_count >= pp->fork_limit) {
        audit_log(AUDIT_FORK_DENIED, "fork limit exceeded");
        return (uint64_t)(int64_t)ERR_PERM;
    }
    process_t* cp = process_create(pp->name, pp->pid);
    if (!cp) return (uint64_t)(int64_t)ERR_NOMEM;
    /* Inherit parent's network namespace */
    if (pp->net_ns && pp->net_ns != &init_net_ns) {
        net_ns_release(cp->net_ns); /* release default from process_create */
        cp->net_ns = pp->net_ns;
        net_ns_retain(cp->net_ns);
    }
    if (pp->cr3 && cp->cr3) {
        err_t e = vmm_duplicate_user_pages(cp->cr3, pp->cr3);
        if (e) return (uint64_t)(int64_t)e;
    }
    cp->entry_point = pp->entry_point;
    cp->user_stack_top = pp->user_stack_top;
    cp->user_code_start = pp->user_code_start;
    cp->user_code_size = pp->user_code_size;
    vma_duplicate(cp);
    thread_t* ct = (thread_t*)PHYS_TO_VIRT(pmm_alloc_page());
    if (!ct) return (uint64_t)(int64_t)ERR_NOMEM;
    kmemset(ct, 0, sizeof(thread_t));
    uint64_t ks_phys = pmm_alloc_pages(
        (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!ks_phys) { pmm_free_page((uint64_t)ct - KERNEL_VMA_BASE); return (uint64_t)(int64_t)ERR_NOMEM; }
    void* ks = (void*)PHYS_TO_VIRT(ks_phys);
    kmemset(ks, 0, THREAD_STACK_SIZE);
    uint64_t kt = (uint64_t)ks + THREAD_STACK_SIZE;
    uint64_t* sp = (uint64_t*)kt;

    /* Build stack top-down: last pushes at lowest address = what switch_context pops first.
     * After switch_context pops 6 callee-saved regs + rets to fork_child_entry,
     * the SP lands on rax=0.  fork_child_entry then pops 15 GP regs (rax..r15),
     * addq $16 (skip vector+error_code), and iretq.
     *
     * Layout from ct->rsp (LOW) upward:
     *   rbp/rbx/r12-r15 = 0  (switch_context pops)
     *   fork_child_entry      (switch_context rets here)
     *   rax=0                 (fork_child_entry pops first)
     *   rbx..r15              (14 more GP regs)
     *   vector, error_code    (skipped by addq $16)
     *   rip, cs, rflags, rsp, ss  (iretq frame, HIGHEST)
     */

    /* 1. iretq frame (highest addresses) */
    *(--sp) = frame->ss;
    *(--sp) = frame->rsp;
    *(--sp) = frame->rflags;
    *(--sp) = frame->cs;
    *(--sp) = frame->rip;

    /* 2. vector + error_code (skipped by fork_child_entry addq $16) */
    *(--sp) = frame->error_code;
    *(--sp) = frame->vector;

    /* 3. GP registers (fork_child_entry pops rax first, then rbx..r15) */
    *(--sp) = frame->r15;
    *(--sp) = frame->r14;
    *(--sp) = frame->r13;
    *(--sp) = frame->r12;
    *(--sp) = frame->r11;
    *(--sp) = frame->r10;
    *(--sp) = frame->r9;
    *(--sp) = frame->r8;
    *(--sp) = frame->rbp;
    *(--sp) = frame->rdi;
    *(--sp) = frame->rsi;
    *(--sp) = frame->rdx;
    *(--sp) = frame->rcx;
    *(--sp) = frame->rbx;
    *(--sp) = 0; /* rax = 0 for child */

    /* 4. switch_context landing: return address + callee-save slots (lowest) */
    *(--sp) = (uint64_t)fork_child_entry;
    *(--sp) = 0; /* r15 for switch_context */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp (closest to sp) */
    ct->rsp = (uint64_t)sp;
    ct->cr3 = cp->cr3;
    ct->state = THREAD_CREATED;
    ct->priority = parent->priority;
    ct->base_priority = parent->base_priority;
    ct->time_slice_remaining = 0;
    ct->kernel_stack = ks;
    ct->kernel_stack_size = THREAD_STACK_SIZE;
    ct->proc = cp;
    kstrncpy(ct->name, pp->name, THREAD_NAME_MAX - 1);
    all_threads_add(ct);
    list_add_tail(&cp->threads, &ct->threads_node);
    cp->thread_count++;
    sched_add_thread(ct);
    pp->fork_count++;
    return cp->pid;
}

static uint64_t sys_sigreturn(int_frame_t* frame) {
    (void)frame;
    /* Restore user context from the sigframe on user stack.
     * rdi points to the sigframe_t pushed by signal delivery.
     * Copy the saved registers back into the interrupt frame.
     */
    sigframe_t sf;
    if (copy_from_user(&sf, (void*)frame->rdi, sizeof(sf)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    /* Restore all registers from the saved signal frame */
    frame->rax  = sf.rax;
    frame->rbx  = sf.rbx;
    frame->rcx  = sf.rcx;
    frame->rdx  = sf.rdx;
    frame->rsi  = sf.rsi;
    frame->rdi  = sf.rdi;
    frame->rbp  = sf.rbp;
    frame->r8   = sf.r8;
    frame->r9   = sf.r9;
    frame->r10  = sf.r10;
    frame->r11  = sf.r11;
    frame->r12  = sf.r12;
    frame->r13  = sf.r13;
    frame->r14  = sf.r14;
    frame->r15  = sf.r15;
    frame->rip  = sf.rip;
    frame->cs   = sf.cs;
    frame->rflags = sf.rflags;
    frame->rsp  = sf.rsp;
    frame->ss   = sf.ss;

    /* We don't return to user via normal iretq here.
     * The frame is modified in place and the ISR's iretq will use it.
     */
    return 0;
}

static uint64_t sys_getcwd(int_frame_t* frame) {
    char* buf = (char*)frame->rdi;
    size_t size = frame->rsi;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc || !buf) return (uint64_t)(int64_t)ERR_INVAL;

    size_t len = kstrlen(proc->cwd) + 1;
    if (len > size) return (uint64_t)(int64_t)ERR_NOSPACE;
    if (copy_to_user(buf, proc->cwd, len) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return len;
}

static uint64_t sys_chdir(int_frame_t* frame) {
    char* path = (char*)frame->rdi;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc || !path) return (uint64_t)(int64_t)ERR_INVAL;

    char kpath[256];
    if (copy_from_user(kpath, path, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    kpath[sizeof(kpath) - 1] = 0;

    /* Verify the path exists via stat */
    vfs_stat_t st;
    if (vfs_stat(kpath, &st) != 0)
        return (uint64_t)(int64_t)ERR_NOENT;

    kstrncpy(proc->cwd, kpath, sizeof(proc->cwd) - 1);
    return 0;
}

static uint64_t sys_dup2(int_frame_t* frame) {
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_INVAL;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_INVAL;
    vfs_fd_t* ft = vfs_get_fd_table();
    if (!ft[oldfd].used) return (uint64_t)(int64_t)ERR_INVAL;

    /* Close newfd if it's open */
    if (ft[newfd].used) {
        vfs_close(newfd);
    }

    /* Duplicate the fd entry */
    ft[newfd] = ft[oldfd];
    ft[newfd].offset = ft[oldfd].offset;
    if (ft[newfd].node)
        __sync_fetch_and_add(&ft[newfd].node->refcount, 1);
    return newfd;
}

static uint64_t sys_clone(int_frame_t* frame) {
    thread_t* parent = current_thread;
    process_t* proc = parent ? parent->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;

    uint64_t flags = frame->rdi;
    uint64_t child_stack_user = frame->rsi;
    (void)flags;

    thread_t* ct = (thread_t*)PHYS_TO_VIRT(pmm_alloc_page());
    if (!ct) return (uint64_t)(int64_t)ERR_NOMEM;
    kmemset(ct, 0, sizeof(thread_t));

    uint64_t ks_phys = pmm_alloc_pages(
        (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!ks_phys) {
        pmm_free_page((uint64_t)ct - KERNEL_VMA_BASE);
        return (uint64_t)(int64_t)ERR_NOMEM;
    }
    void* ks = (void*)PHYS_TO_VIRT(ks_phys);
    kmemset(ks, 0, THREAD_STACK_SIZE);
    uint64_t kt = (uint64_t)ks + THREAD_STACK_SIZE;
    uint64_t* sp = (uint64_t*)kt;

    /* 1. iretq frame (highest addresses) */
    *(--sp) = frame->ss;
    *(--sp) = child_stack_user ? child_stack_user : frame->rsp;
    *(--sp) = frame->rflags;
    *(--sp) = frame->cs;
    *(--sp) = frame->rip;

    /* 2. vector + error_code (skipped by fork_child_entry addq $16) */
    *(--sp) = frame->error_code;
    *(--sp) = frame->vector;

    /* 3. GP registers (fork_child_entry pops rax first, then rbx..r15) */
    *(--sp) = frame->r15;
    *(--sp) = frame->r14;
    *(--sp) = frame->r13;
    *(--sp) = frame->r12;
    *(--sp) = frame->r11;
    *(--sp) = frame->r10;
    *(--sp) = frame->r9;
    *(--sp) = frame->r8;
    *(--sp) = frame->rbp;
    *(--sp) = frame->rdi;
    *(--sp) = frame->rsi;
    *(--sp) = frame->rdx;
    *(--sp) = frame->rcx;
    *(--sp) = frame->rbx;
    *(--sp) = 0; /* rax = 0 for child */

    /* 4. switch_context landing: return address + callee-save slots (lowest) */
    *(--sp) = (uint64_t)fork_child_entry;
    *(--sp) = 0; /* r15 for switch_context */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp (closest to sp) */

    ct->rsp = (uint64_t)sp;
    ct->cr3 = proc->cr3; /* Same address space */
    ct->state = THREAD_CREATED;
    ct->priority = parent->priority;
    ct->base_priority = parent->base_priority;
    ct->time_slice_remaining = 0;
    ct->kernel_stack = ks;
    ct->kernel_stack_size = THREAD_STACK_SIZE;
    ct->proc = proc;
    kstrncpy(ct->name, proc->name, THREAD_NAME_MAX - 1);
    all_threads_add(ct);
    list_add_tail(&proc->threads, &ct->threads_node);
    proc->thread_count++;
    sched_add_thread(ct);

    return proc->pid; /* Parent returns child PID */
}

static uint64_t sys_kill(int_frame_t* frame) {
    pid_t pid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;
    if (sig < 0 || sig >= NSIG) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* self = current_thread ? current_thread->proc : NULL;
    /* Allow signaling self without capability check */
    if (pid != (self ? self->pid : 0) && !cap_check(CAP_KILL))
        return (uint64_t)(int64_t)ERR_PERM;
    process_t* p = process_find(pid);
    if (!p) return (uint64_t)(int64_t)ERR_NOENT;
    signal_send(pid, sig);
    signal_process(p);
    return 0;
}

static uint64_t sys_sigaction(int_frame_t* frame) {
    int sig = (int)frame->rdi;
    sigaction_t* new_act = (sigaction_t*)frame->rsi;
    sigaction_t* old_act = (sigaction_t*)frame->rdx;
    if (sig < 0 || sig >= NSIG) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;

    if (old_act) {
        sigaction_t old = proc->signal_actions[sig];
        if (copy_to_user(old_act, &old, sizeof(old)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    if (new_act) {
        sigaction_t act;
        if (copy_from_user(&act, new_act, sizeof(act)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        proc->signal_actions[sig] = act;
    }
    return 0;
}

static uint64_t sys_waitpid(int_frame_t* frame) {
    pid_t pid = (pid_t)frame->rdi;
    int* status = (int*)frame->rsi;
    int options = (int)frame->rdx;
    (void)options;
    if (pid == 0) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* p = process_find(pid);
    if (!p) return (uint64_t)(int64_t)ERR_NOENT;

    while (!p->exited) {
        /* If WUNTRACED, return for stopped processes */
        if ((options & WUNTRACED) && (p->flags & PROC_FLAG_STOPPED)) {
            if (status) {
                int code = W_STOPCODE(SIGTSTP);
                if (copy_to_user(status, &code, sizeof(code)) != 0)
                    return (uint64_t)(int64_t)ERR_FAULT;
            }
            return pid;
        }
        sched_block(&p->exit_waiters);
    }

    if (status) {
        int code = p->exit_code;
        if (copy_to_user(status, &code, sizeof(code)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    process_reap(p);
    return pid;
}

static uint64_t sys_getppid(int_frame_t* frame) {
    (void)frame;
    if (current_thread && current_thread->proc)
        return current_thread->proc->ppid;
    return 0;
}

static uint64_t sys_yield(int_frame_t* frame) {
    (void)frame;
    thread_yield();
    return 0;
}

static uint64_t sys_sleep(int_frame_t* frame) {
    uint64_t ms = frame->rdi;
    thread_sleep(ms);
    return 0;
}

static uint64_t sys_uptime(int_frame_t* frame) {
    (void)frame;
    uint64_t ticks = hal_timer_get_ticks();
    uint64_t hz = hal_timer_get_hz();
    return ticks * 1000 / hz;
}

static uint64_t sys_reboot(int_frame_t* frame) {
    (void)frame;
    if (!cap_check(CAP_SYS_BOOT)) return (uint64_t)(int64_t)ERR_PERM;
    audit_log(AUDIT_SENSITIVE_CALL, "reboot");
    hal_reboot();
    return 0;
}

static uint64_t sys_pwrdown(int_frame_t* frame) {
    (void)frame;
    if (!cap_check(CAP_SYS_BOOT)) return (uint64_t)(int64_t)ERR_PERM;
    audit_log(AUDIT_SENSITIVE_CALL, "poweroff");
    hal_poweroff();
    return 0;
}

static uint64_t sys_create(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    int r = vfs_create(kernel_path, 0);
    if (r < 0) return (uint64_t)(int64_t)ERR_IO;
    return (uint64_t)r;
}

static uint64_t sys_mkdir(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    int r = vfs_mkdir(kernel_path);
    if (r < 0) return (uint64_t)(int64_t)ERR_IO;
    return 0;
}

static uint64_t sys_unlink(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    int r = vfs_unlink(kernel_path);
    if (r < 0) return (uint64_t)(int64_t)ERR_IO;
    return 0;
}

static uint64_t sys_pipe(int_frame_t* frame) {
    int* fds = (int*)frame->rdi;
    int kfds[2];
    if (pipe_create(kfds) != 0)
        return (uint64_t)(int64_t)ERR_IO;
    if (copy_to_user(fds, kfds, sizeof(kfds)) != 0) {
        vfs_close(kfds[0]);
        vfs_close(kfds[1]);
        return (uint64_t)(int64_t)ERR_FAULT;
    }
    return 0;
}

static uint64_t sys_stat(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    vfs_stat_t* st = (vfs_stat_t*)frame->rsi;
    char kernel_path[256];
    if (copy_path_from_user(path, kernel_path, sizeof(kernel_path)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    vfs_stat_t kst;
    if (vfs_stat(kernel_path, &kst) != 0)
        return (uint64_t)(int64_t)ERR_NOENT;
    if (st && copy_to_user(st, &kst, sizeof(kst)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return 0;
}

static uint64_t sys_lseek(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    int64_t offset = (int64_t)frame->rsi;
    int whence = (int)frame->rdx;
    int64_t ret = vfs_lseek(fd, offset, whence);
    if (ret < 0) return (uint64_t)(int64_t)ERR_INVAL;
    return (uint64_t)ret;
}

static uint64_t sys_mmap(int_frame_t* frame) {
    uint64_t addr    = frame->rdi;
    size_t   len     = frame->rsi;
    int      prot    = (int)frame->rdx;
    int      flags   = (int)frame->r10;
    int      fd      = (int)frame->r8;
    uint64_t offset  = frame->r9;

    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;

    size_t page_len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (page_len == 0) return (uint64_t)(int64_t)ERR_INVAL;

    int is_anon = (flags & 0x20) != 0; /* MAP_ANONYMOUS */
    vfs_node_t* node = NULL;

    if (!is_anon) {
        /* File-backed mapping: validate fd */
        if (fd < 0 || fd >= MAX_FDS) return (uint64_t)(int64_t)ERR_BADFD;
        vfs_fd_t* ft = vfs_get_fd_table();
        if (!ft[fd].used) return (uint64_t)(int64_t)ERR_BADFD;
        node = ft[fd].node;
        if (!node) return (uint64_t)(int64_t)ERR_BADFD;
        int acc_mode = ft[fd].flags & 3;
        /* Must be open for reading */
        if (acc_mode == 1) return (uint64_t)(int64_t)ERR_PERM; /* O_WRONLY */
        /* MAP_SHARED + PROT_WRITE needs write permission */
        if ((flags & 0x01) && (prot & 0x02) && acc_mode != 2)
            return (uint64_t)(int64_t)ERR_PERM; /* not O_RDWR */
        /* Offset must be page-aligned */
        if (offset & 0xFFF) return (uint64_t)(int64_t)ERR_INVAL;
        /* Must have read op */
        if (!node->fs || !node->fs->ops || !node->fs->ops->read)
            return (uint64_t)(int64_t)ERR_PERM;
        /* For MAP_SHARED+PROT_WRITE, need write op */
        if ((flags & 0x01) && (prot & 0x02) &&
            (!node->fs->ops->write))
            return (uint64_t)(int64_t)ERR_PERM;
    }

    uint64_t vaddr;
    if (flags & 0x10) { /* MAP_FIXED */
        if (addr & 0xFFF) return (uint64_t)(int64_t)ERR_INVAL;
        if (addr < USER_VIRT_START || addr + page_len > USER_VIRT_END)
            return (uint64_t)(int64_t)ERR_INVAL;
        /* Remove VMA-backed mappings and unmap any direct page mappings */
        vma_remove(proc, addr, page_len);
        for (uint64_t p = addr; p < addr + page_len; p += PAGE_SIZE) {
            page_entry_t* pte = vmm_walk_pagetable(proc->cr3, p);
            if (pte && (*pte & PAGE_PRESENT)) {
                uint64_t phys = *pte & ~0xFFFULL;
                pmm_free_page(phys);
                vmm_unmap_page(proc->cr3, p);
            }
        }
        vaddr = addr;
    } else {
        vaddr = proc->mmap_brk;
        proc->mmap_brk += page_len;
        if (proc->mmap_brk > 0x7FFF0000) return (uint64_t)(int64_t)ERR_NOMEM;
    }

    /* Create VMA */
    vma_t* v = vma_add(proc, vaddr, vaddr + page_len, prot, flags, node, offset);
    if (!v) return (uint64_t)(int64_t)ERR_NOMEM;

    if (is_anon) {
        /* Anonymous: allocate pages immediately (existing behavior) */
        uint64_t pgfl = PAGE_USER;
        if (prot & 0x2) pgfl |= PAGE_WRITE;
        for (uint64_t p = vaddr; p < vaddr + page_len; p += PAGE_SIZE) {
            page_entry_t* pte = vmm_walk_pagetable(proc->cr3, p);
            if (pte && (*pte & PAGE_PRESENT)) continue;
            uint64_t phys = pmm_alloc_page();
            if (!phys) return (uint64_t)(int64_t)ERR_NOMEM;
            kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            vmm_map_page(proc->cr3, p, phys, pgfl);
        }
    }
    /* File-backed: demand-paged via page fault handler */

    return vaddr;
}

static uint64_t sys_munmap(int_frame_t* frame) {
    uint64_t addr = frame->rdi;
    size_t   len  = frame->rsi;
    if (addr & 0xFFF) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    return (uint64_t)(int64_t)vma_remove(proc, addr, len);
}

static uint64_t sys_mprotect(int_frame_t* frame) {
    uint64_t addr = frame->rdi;
    size_t   len  = frame->rsi;
    int      prot = (int)frame->rdx;
    if (addr & 0xFFF) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    size_t page_len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = addr + page_len;

    /* Update VMA prot flags for matching VMAs */
    {
        cpu_flags_t _vf;
        spinlock_acquire(&proc->vma_lock, &_vf);
        vma_t* v = (vma_t*)proc->vmas;
        while (v) {
            uint64_t inter_start = (addr > v->start) ? addr : v->start;
            uint64_t inter_end   = (end < v->end) ? end : v->end;
            if (inter_start < inter_end)
                v->prot = prot;
            v = v->next;
        }
        spinlock_release(&proc->vma_lock, _vf);
    }

    /* Update page table entries */
    for (uint64_t p = addr; p < end; p += PAGE_SIZE) {
        page_entry_t* pte = vmm_walk_pagetable(proc->cr3, p);
        if (!pte || !(*pte & PAGE_PRESENT)) continue;
        uint64_t base = *pte & ~0xFFFULL;
        uint64_t flags = PAGE_USER | PAGE_PRESENT;
        if (prot & 0x2) flags |= PAGE_WRITE;
        *pte = base | flags;
        vmm_flush_tlb_page(p);
    }
    return 0;
}

static uint64_t sys_ioctl(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint64_t request = frame->rsi;
    void* argp = (void*)frame->rdx;

    /* Try socket ioctl first */
    socket_t* s = sock_lookup(fd);
    if (s) {
        int ret = sock_ioctl(s, request, argp);
        return (uint64_t)(int64_t)ret;
    }

    int ret = vfs_ioctl(fd, request, argp);
    if (ret < 0) return (uint64_t)(int64_t)ERR_INVAL;
    return 0;
}

static uint64_t sys_setpgid(int_frame_t* frame) {
    pid_t pid = (pid_t)frame->rdi;
    pid_t pgid = (pid_t)frame->rsi;
    process_t* proc;
    if (pid == 0) {
        proc = current_thread ? current_thread->proc : NULL;
    } else {
        proc = process_find(pid);
    }
    if (!proc) return (uint64_t)(int64_t)ERR_NOENT;
    /* Only allow setting pgid on own process or child */
    process_t* self = current_thread ? current_thread->proc : NULL;
    if (proc != self && proc->ppid != (self ? self->pid : 0))
        return (uint64_t)(int64_t)ERR_PERM;
    if (pgid == 0)
        pgid = proc->pid;
    proc->pgid = pgid;
    return 0;
}

static uint64_t sys_getpgid(int_frame_t* frame) {
    pid_t pid = (pid_t)frame->rdi;
    process_t* proc;
    if (pid == 0) {
        proc = current_thread ? current_thread->proc : NULL;
    } else {
        proc = process_find(pid);
    }
    if (!proc) return (uint64_t)(int64_t)ERR_NOENT;
    return (uint64_t)proc->pgid;
}

static uint64_t sys_pty_pair(int_frame_t* frame) {
    int* fds = (int*)frame->rdi;
    int kfds[2];
    if (pty_pair_create(kfds) != 0)
        return (uint64_t)(int64_t)ERR_IO;
    if (copy_to_user(fds, kfds, sizeof(kfds)) != 0) {
        vfs_close(kfds[0]);
        vfs_close(kfds[1]);
        return (uint64_t)(int64_t)ERR_FAULT;
    }
    return 0;
}

/* ---- Socket syscalls ---- */

/* Translate user-space AF_* to kernel AF_* */
static int af_from_user(int user_af) {
    switch (user_af) {
    case 1:  return AF_UNIX;   /* user AF_UNIX → kernel AF_UNIX (1) */
    case 2:  return AF_INET;   /* user AF_INET → kernel AF_INET (4) */
    case 10: return AF_INET6;  /* user AF_INET6 → kernel AF_INET6 (6) */
    default: return AF_UNSPEC;
    }
}

static uint64_t sys_socket(int_frame_t* frame) {
    int user_af  = (int)frame->rdi;
    int type     = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int af = af_from_user(user_af);
    if (af == AF_UNSPEC) return (uint64_t)(int64_t)ERR_INVAL;

    socket_t* s = socket_alloc(af, type, protocol);
    if (!s) return (uint64_t)(int64_t)ERR_NOMEM;

    int fd = sock_register(s);
    if (fd < 0) { socket_release(s); return (uint64_t)(int64_t)ERR_NOSPACE; }

    KDEBUG("[SYSCALL] socket(af=%d type=%d proto=%d) → fd=%d\n", user_af, type, protocol, fd);
    return (uint64_t)fd;
}

static uint64_t sys_bind(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const sockaddr_t* user_addr = (const sockaddr_t*)frame->rsi;
    socklen_t addrlen = (socklen_t)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    /* Copy sockaddr from user space */
    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;
    if (addrlen > SOCKADDR_MAX) addrlen = SOCKADDR_MAX;
    if (addrlen < sizeof(uint16_t)) return (uint64_t)(int64_t)ERR_INVAL;
    if (copy_from_user(kaddr, user_addr, addrlen) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    int e = sock_bind(s, kaddr, addrlen);
    KDEBUG("[SYSCALL] bind(fd=%d) → %d\n", fd, e);
    return (uint64_t)(int64_t)e;
}

static uint64_t sys_connect(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const sockaddr_t* user_addr = (const sockaddr_t*)frame->rsi;
    socklen_t addrlen = (socklen_t)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;
    if (addrlen > SOCKADDR_MAX) addrlen = SOCKADDR_MAX;
    if (addrlen < sizeof(uint16_t)) return (uint64_t)(int64_t)ERR_INVAL;
    if (copy_from_user(kaddr, user_addr, addrlen) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    KDEBUG("[SYSCALL] connect(fd=%d)\n", fd);
    int e = sock_connect(s, kaddr, addrlen);
    KDEBUG("[SYSCALL] connect → %d\n", e);
    return (uint64_t)(int64_t)e;
}

static uint64_t sys_listen(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    int backlog = (int)frame->rsi;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    int e = sock_listen(s, backlog);
    kprintf("[SYSCALL] listen(fd=%d backlog=%d) → %d\n", fd, backlog, e);
    return (uint64_t)(int64_t)e;
}

static uint64_t sys_accept(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    sockaddr_t* user_addr = (sockaddr_t*)frame->rsi;
    socklen_t* user_addrlen = (socklen_t*)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    socklen_t addrlen = SOCKADDR_MAX;
    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;

    kprintf("[SYSCALL] accept(fd=%d)\n", fd);
    socket_t* client = sock_accept(s, kaddr, &addrlen);
    if (!client) return (uint64_t)(int64_t)ERR_AGAIN;

    int client_fd = sock_register(client);
    if (client_fd < 0) { socket_release(client); return (uint64_t)(int64_t)ERR_NOSPACE; }

    kprintf("[SYSCALL] accept → client_fd=%d\n", client_fd);

    /* Copy peer address back to user */
    if (user_addr && user_addrlen) {
        socklen_t user_len;
        if (copy_from_user(&user_len, user_addrlen, sizeof(user_len)) != 0) {
            sock_unregister(client_fd);
            socket_release(client);
            return (uint64_t)(int64_t)ERR_FAULT;
        }
        socklen_t copy_len = addrlen < user_len ? addrlen : user_len;
        if (copy_to_user(user_addr, kaddr, copy_len) != 0) {
            sock_unregister(client_fd);
            socket_release(client);
            return (uint64_t)(int64_t)ERR_FAULT;
        }
        if (copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) != 0) {
            sock_unregister(client_fd);
            socket_release(client);
            return (uint64_t)(int64_t)ERR_FAULT;
        }
    }

    return (uint64_t)client_fd;
}

static uint64_t sys_send(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const uint8_t* buf = (const uint8_t*)frame->rsi;
    uint32_t len = (uint32_t)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t kbuf[2048];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    if (copy_from_user(kbuf, buf, len) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    int ret = sock_send(s, kbuf, len);
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_recv(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint8_t* buf = (uint8_t*)frame->rsi;
    uint32_t size = (uint32_t)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t kbuf[2048];
    if (size > sizeof(kbuf)) size = sizeof(kbuf);

    int ret = sock_recv(s, kbuf, size);
    if (ret > 0) {
        if (copy_to_user(buf, kbuf, (uint32_t)ret) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_sendto(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const uint8_t* buf = (const uint8_t*)frame->rsi;
    uint32_t len = (uint32_t)frame->rdx;
    (void)frame->r10; /* flags */
    const sockaddr_t* user_dst = (const sockaddr_t*)frame->r8;
    socklen_t addrlen = (socklen_t)frame->r9;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t kbuf[2048];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    if (copy_from_user(kbuf, buf, len) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;
    if (addrlen > SOCKADDR_MAX) addrlen = SOCKADDR_MAX;
    if (addrlen < sizeof(uint16_t)) return (uint64_t)(int64_t)ERR_INVAL;
    if (copy_from_user(kaddr, user_dst, addrlen) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    int ret = sock_sendto(s, kbuf, len, kaddr, addrlen);
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_recvfrom(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint8_t* buf = (uint8_t*)frame->rsi;
    uint32_t size = (uint32_t)frame->rdx;
    (void)frame->r10; /* flags */
    sockaddr_t* user_src = (sockaddr_t*)frame->r8;
    socklen_t* user_addrlen = (socklen_t*)frame->r9;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t kbuf[2048];
    if (size > sizeof(kbuf)) size = sizeof(kbuf);

    socklen_t addrlen = SOCKADDR_MAX;
    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;

    int ret = sock_recvfrom(s, kbuf, size, kaddr, &addrlen);
    if (ret > 0) {
        if (copy_to_user(buf, kbuf, (uint32_t)ret) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;

        if (user_src && user_addrlen) {
            socklen_t user_len;
            if (copy_from_user(&user_len, user_addrlen, sizeof(user_len)) != 0)
                return (uint64_t)(int64_t)ERR_FAULT;
            socklen_t copy_len = addrlen < user_len ? addrlen : user_len;
            if (copy_to_user(user_src, kaddr, copy_len) != 0)
                return (uint64_t)(int64_t)ERR_FAULT;
            if (copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) != 0)
                return (uint64_t)(int64_t)ERR_FAULT;
        }
    }
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_setsockopt(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    const void* optval = (const void*)frame->r10;
    socklen_t optlen = (socklen_t)frame->r8;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    uint8_t koptval[128];
    if (optlen > sizeof(koptval)) optlen = sizeof(koptval);
    if (optval && copy_from_user(koptval, optval, optlen) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    int ret = sock_setsockopt(s, level, optname, koptval, optlen);
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_getsockopt(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    void* optval = (void*)frame->r10;
    socklen_t* user_optlen = (socklen_t*)frame->r8;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    socklen_t optlen = 0;
    if (user_optlen && copy_from_user(&optlen, user_optlen, sizeof(optlen)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    uint8_t koptval[128];
    if (optlen > sizeof(koptval)) optlen = sizeof(koptval);

    int ret = sock_getsockopt(s, level, optname, koptval, &optlen);
    if (ret < 0) return (uint64_t)(int64_t)ret;

    if (optval && optlen > 0 && copy_to_user(optval, koptval, optlen) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    if (user_optlen && copy_to_user(user_optlen, &optlen, sizeof(optlen)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_getsockname(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    sockaddr_t* user_addr = (sockaddr_t*)frame->rsi;
    socklen_t* user_len = (socklen_t*)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    socklen_t len = SOCKADDR_MAX;
    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;

    int ret = sock_getsockname(s, kaddr, &len);
    if (ret < 0) return (uint64_t)(int64_t)ret;

    if (user_addr && user_len) {
        socklen_t user_len_val;
        if (copy_from_user(&user_len_val, user_len, sizeof(user_len_val)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        socklen_t copy_len = len < user_len_val ? len : user_len_val;
        if (copy_to_user(user_addr, kaddr, copy_len) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        if (copy_to_user(user_len, &len, sizeof(len)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    return 0;
}

static uint64_t sys_getpeername(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    sockaddr_t* user_addr = (sockaddr_t*)frame->rsi;
    socklen_t* user_len = (socklen_t*)frame->rdx;

    socket_t* s = sock_lookup(fd);
    if (!s) return (uint64_t)(int64_t)ERR_BADFD;

    socklen_t len = SOCKADDR_MAX;
    uint8_t kaddr_buf[SOCKADDR_MAX];
    sockaddr_t* kaddr = (sockaddr_t*)kaddr_buf;

    int ret = sock_getpeername(s, kaddr, &len);
    if (ret < 0) return (uint64_t)(int64_t)ret;

    if (user_addr && user_len) {
        socklen_t user_len_val;
        if (copy_from_user(&user_len_val, user_len, sizeof(user_len_val)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        socklen_t copy_len = len < user_len_val ? len : user_len_val;
        if (copy_to_user(user_addr, kaddr, copy_len) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        if (copy_to_user(user_len, &len, sizeof(len)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    return 0;
}

/* POSIX pollfd structure (userspace-visible layout) */
struct pollfd {
    int fd;
    short events;
    short revents;
};

static uint64_t sys_poll(int_frame_t* frame) {
    struct pollfd* user_fds = (struct pollfd*)frame->rdi;
    int nfds = (int)frame->rsi;
    int timeout_ms = (int)frame->rdx;
    (void)timeout_ms;

    if (nfds <= 0) return (uint64_t)(int64_t)ERR_INVAL;
    if (nfds > 64) nfds = 64;

    /* Copy pollfd array from userspace */
    struct pollfd kfds[64];
    if (copy_from_user(kfds, user_fds, nfds * sizeof(struct pollfd)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        kfds[i].revents = 0;
        if (kfds[i].fd < 0) {
            kfds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        socket_t* s = sock_lookup(kfds[i].fd);
        if (s) {
            sock_poll(s, kfds[i].events, (int*)&kfds[i].revents);
            if (kfds[i].revents) ready++;
        } else {
            /* Non-socket fds: assume writable (VFS pipe/tty) */
            if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
            if (kfds[i].revents) ready++;
        }
    }

    if (copy_to_user(user_fds, kfds, nfds * sizeof(struct pollfd)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    return (uint64_t)ready;
}

static uint64_t sys_clock_gettime(int_frame_t* frame) {
    int clk_id = (int)frame->rdi;
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } ts;
    (void)clk_id;

    if (clk_id == 0) {
        uint64_t now = ntp_get_time();
        if (now == 0) return (uint64_t)(int64_t)ERR_NOSYS;
        ts.tv_sec = (int64_t)now;
        ts.tv_nsec = (int64_t)(hal_timer_get_ns() % 1000000000ULL);
    } else {
        uint64_t ns = hal_timer_get_ns();
        ts.tv_sec = (int64_t)(ns / 1000000000ULL);
        ts.tv_nsec = (int64_t)(ns % 1000000000ULL);
    }

    if (copy_to_user((void*)frame->rsi, &ts, sizeof(ts)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return 0;
}

static uint64_t sys_fcntl(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    int cmd = (int)frame->rsi;
    long arg = (long)frame->rdx;

    if (fd < 0 || fd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_BADFD;

    vfs_fd_t* ft = vfs_get_fd_table();
    if (!ft[fd].used) return (uint64_t)(int64_t)ERR_BADFD;

    switch (cmd) {
    case 0: { /* F_DUPFD */
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_INVAL;
        int newfd = -1;
        for (int i = minfd; i < VFS_MAX_FDS; i++) {
            if (!ft[i].used) { newfd = i; break; }
        }
        if (newfd < 0) return (uint64_t)(int64_t)ERR_NOSPACE;
        ft[newfd] = ft[fd];
        ft[newfd].offset = ft[fd].offset;
        if (ft[newfd].node)
            __sync_fetch_and_add(&ft[newfd].node->refcount, 1);
        return (uint64_t)newfd;
    }
    case 1: /* F_GETFD */
        return (uint64_t)(int64_t)0; /* no cloexec support yet */
    case 2: /* F_SETFD */
        return 0; /* ignore cloexec for now */
    case 3: /* F_GETFL */
        return (uint64_t)(int64_t)ft[fd].flags;
    case 4: /* F_SETFL */
        ft[fd].flags = (ft[fd].flags & ~(O_APPEND | O_NONBLOCK))
                     | (arg & (O_APPEND | O_NONBLOCK));
        return 0;
    default:
        return (uint64_t)(int64_t)ERR_INVAL;
    }
}

static uint64_t sys_capget(int_frame_t* frame) {
    process_t* self = current_thread ? current_thread->proc : NULL;
    if (!self) return (uint64_t)(int64_t)ERR_INVAL;
    pid_t pid = (pid_t)frame->rdi;
    uint64_t* user_caps = (uint64_t*)frame->rsi;
    process_t* p = (pid == 0 || pid == self->pid) ? self : process_find(pid);
    if (!p) return (uint64_t)(int64_t)ERR_NOENT;
    if (user_caps) {
            if (!is_user_range_valid((uint64_t)user_caps, sizeof(uint64_t)))
            return (uint64_t)(int64_t)ERR_FAULT;
        smap_disable();
        *user_caps = p->caps;
        smap_enable();
    }
    return 0;
}

static uint64_t sys_capset(int_frame_t* frame) {
    process_t* self = current_thread ? current_thread->proc : NULL;
    if (!self) return (uint64_t)(int64_t)ERR_INVAL;
    uint64_t caps = frame->rdi;
    /* Can only drop capabilities, not add them */
    self->caps &= caps;
    return 0;
}

static uint64_t sys_audit_read(int_frame_t* frame) {
    audit_entry_t* user_buf = (audit_entry_t*)frame->rdi;
    size_t count = (size_t)frame->rsi;
    if (!count) return 0;
    if (!is_user_range_valid((uint64_t)user_buf, count * sizeof(audit_entry_t)))
        return (uint64_t)(int64_t)ERR_FAULT;
    if (count > (AUDIT_BUFFER_SIZE / 4))
        count = (AUDIT_BUFFER_SIZE / 4);
    size_t n = 0;
    audit_entry_t tmp;
    for (size_t i = 0; i < count; i++) {
        if (audit_read_next(&tmp) != ERR_OK)
            break;
        smap_disable();
        kmemcpy(&user_buf[i], &tmp, sizeof(audit_entry_t));
        smap_enable();
        n++;
    }
    return n;
}

/* ── UID/GID syscalls ── */
static uint64_t sys_getuid(int_frame_t* frame) {
    (void)frame;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    return proc->uid;
}

static uint64_t sys_geteuid(int_frame_t* frame) {
    (void)frame;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    return proc->euid;
}

static uint64_t sys_getgid(int_frame_t* frame) {
    (void)frame;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    return proc->gid;
}

static uint64_t sys_getegid(int_frame_t* frame) {
    (void)frame;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    return proc->egid;
}

static uint64_t sys_setuid(int_frame_t* frame) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    uid_t uid = (uid_t)frame->rdi;
    /* Only root can set arbitrary uid; non-root can set to euid/ruid */
    if (proc->uid != 0 && uid != proc->uid && uid != proc->euid)
        return (uint64_t)(int64_t)ERR_PERM;
    if (!cap_check(CAP_SYS_SETUID)) return (uint64_t)(int64_t)ERR_PERM;
    proc->uid = proc->euid = uid;
    return 0;
}

static uint64_t sys_setgid(int_frame_t* frame) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    gid_t gid = (gid_t)frame->rdi;
    if (proc->gid != 0 && gid != proc->gid && gid != proc->egid)
        return (uint64_t)(int64_t)ERR_PERM;
    if (!cap_check(CAP_SYS_SETUID)) return (uint64_t)(int64_t)ERR_PERM;
    proc->gid = proc->egid = gid;
    return 0;
}

/* ── getrandom ── */
static uint64_t sys_getrandom(int_frame_t* frame) {
    void* buf = (void*)frame->rdi;
    size_t count = (size_t)frame->rsi;
    unsigned int flags = (unsigned int)frame->rdx;
    (void)flags;
    if (!count) return 0;
    if (!is_user_range_valid((uint64_t)buf, count))
        return (uint64_t)(int64_t)ERR_FAULT;
    /* Generate directly into user buffer (SMAP-safe) */
    uint8_t tmp[64];
    size_t written = 0;
    while (written < count) {
        size_t chunk = count - written;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        random_get_bytes(tmp, chunk);
        smap_disable();
        kmemcpy((uint8_t*)buf + written, tmp, chunk);
        smap_enable();
        written += chunk;
    }
    return written;
}

/* ── Set syscall filter ── */
static uint64_t sys_set_ssf(int_frame_t* frame) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    const uint64_t* user_mask = (const uint64_t*)frame->rdi;
    size_t count = (size_t)frame->rsi;
    if (!user_mask || count == 0) return (uint64_t)(int64_t)ERR_INVAL;
    if (!is_user_range_valid((uint64_t)user_mask, count * sizeof(uint64_t)))
        return (uint64_t)(int64_t)ERR_FAULT;
    if (count > 4) count = 4;
    /* Read mask from userspace */
    uint64_t kbuf[4];
    smap_disable();
    kmemcpy(kbuf, user_mask, count * sizeof(uint64_t));
    smap_enable();
    /* Can only drop permissions (clear bits), not add them */
    for (size_t i = 0; i < count; i++)
        proc->syscall_mask[i] &= kbuf[i];
    return 0;
}

/* ── socketpair ── */
static uint64_t sys_socketpair(int_frame_t* frame) {
    int domain = (int)frame->rdi;
    int type   = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int* user_sv = (int*)frame->r10;

    if (domain != 1) return (uint64_t)(int64_t)ERR_INVAL; /* AF_UNIX only */
    if (type != 1)  return (uint64_t)(int64_t)ERR_INVAL;  /* SOCK_STREAM only */
    (void)protocol;

    if (!is_user_range_valid((uint64_t)user_sv, 2 * sizeof(int)))
        return (uint64_t)(int64_t)ERR_FAULT;

    int sv[2];
    int ret = unix_socketpair(sv);
    if (ret < 0) return (uint64_t)(int64_t)ret;

    smap_disable();
    user_sv[0] = sv[0];
    user_sv[1] = sv[1];
    smap_enable();
    return 0;
}

/* ── prctl ── */
static uint64_t sys_prctl(int_frame_t* frame) {
    int option = (int)frame->rdi;
    unsigned long arg2 = frame->rsi;
    (void)frame->rdx; (void)frame->r10; (void)frame->r8; (void)frame->r9;

    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;

    switch (option) {
    case 0: /* PR_SET_NO_NEW_PRIVS (value=1) */
        if (arg2 != 1) return (uint64_t)(int64_t)ERR_INVAL;
        proc->no_new_privs = 1;
        return 0;
    case 1: /* PR_GET_NO_NEW_PRIVS */
        return proc->no_new_privs ? 1 : 0;
    default:
        return (uint64_t)(int64_t)ERR_NOSYS;
    }
}

/* ── secure_boot syscall ── */
static uint64_t sys_veth_move(int_frame_t* frame) {
    int pair_idx = (int)frame->rdi;
    int end_sel  = (int)frame->rsi;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;
    return (uint64_t)(int64_t)veth_end_move(pair_idx, end_sel, proc->net_ns);
}

static uint64_t sys_unshare(int_frame_t* frame) {
    uint64_t flags = frame->rdi;
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return (uint64_t)(int64_t)ERR_INVAL;

    if (flags & CLONE_NEWNET) {
        net_ns_t* new_ns = net_ns_alloc();
        if (!new_ns) return (uint64_t)(int64_t)ERR_NOMEM;
        kmemcpy(new_ns->name, "netns", 6);
        net_ns_release(proc->net_ns);
        proc->net_ns = new_ns;
        audit_log(AUDIT_NETNS_CREATE, "unshare(CLONE_NEWNET)");
    }

    return ERR_OK;
}

static uint64_t sys_veth_pair(int_frame_t* frame) {
    /* rdi: netconfig_req_t* user_req — pass same struct to get MACs back */
    netconfig_req_t* user_req = (netconfig_req_t*)frame->rdi;
    int idx;
    err_t e = veth_pair_create(&idx);
    if (e != ERR_OK) return (uint64_t)(int64_t)e;

    if (user_req) {
        /* Copy veth MACs back to user */
        netconfig_req_t kresp;
        kmemset(&kresp, 0, sizeof(kresp));
        kresp.op = idx; /* store pair index */
        kmemcpy(kresp.mac, veth_get_mac(idx, 0), 6);
        kmemcpy(kresp.addr6, veth_get_mac(idx, 1), 6); /* reuse addr6 for second MAC */
        if (copy_to_user(user_req, &kresp, sizeof(kresp)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    return (uint64_t)(int64_t)idx;
}

static uint64_t sys_netconfig(int_frame_t* frame) {
    netconfig_req_t* user_req = (netconfig_req_t*)frame->rdi;
    netconfig_req_t kreq;
    if (copy_from_user(&kreq, user_req, sizeof(kreq)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    switch (kreq.op) {
    case NETCONFIG_SET_IPV4:
        ipv4_set_addr_prefix(kreq.addr4, kreq.prefix_len);
        return ERR_OK;
    case NETCONFIG_ADD_ROUTE_V4:
        return (uint64_t)(int64_t)route_add_v4(kreq.addr4, kreq.prefix_len, kreq.gw4);
    case NETCONFIG_DEL_ROUTE_V4:
        return (uint64_t)(int64_t)route_del_v4(kreq.addr4, kreq.prefix_len);
    case NETCONFIG_SET_ARP:
        arp_set(kreq.addr4, kreq.mac);
        return ERR_OK;
    case NETCONFIG_DEL_ARP:
        return (uint64_t)(int64_t)arp_del(kreq.addr4);
    case NETCONFIG_SET_IPV6: {
        net_ns_t* ns = get_current_ns();
        kmemcpy(ns->global_ipv6, kreq.addr6, 16);
        kprintf("[NETCONFIG] IPv6 address set\n");
        return ERR_OK;
    }
    case NETCONFIG_ADD_ROUTE_V6:
        return (uint64_t)(int64_t)route_add_v6(kreq.addr6, kreq.prefix_len, kreq.gw6);
    case NETCONFIG_DEL_ROUTE_V6:
        return (uint64_t)(int64_t)route_del_v6(kreq.addr6, kreq.prefix_len);
    case NETCONFIG_SET_NDP:
        ndp_cache_update(kreq.addr6, kreq.mac);
        return ERR_OK;
    case NETCONFIG_DEL_NDP:
        return (uint64_t)(int64_t)ndp_cache_delete(kreq.addr6);
    case NETCONFIG_GET_IPV4: {
        ipv4_addr_t a = ipv4_get_addr();
        netconfig_req_t resp;
        kmemset(&resp, 0, sizeof(resp));
        resp.addr4 = a;
        resp.prefix_len = ipv4_get_prefix_len();
        if (copy_to_user(user_req, &resp, sizeof(resp)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        return ERR_OK;
    }
    default:
        return (uint64_t)(int64_t)ERR_INVAL;
    }
}

static uint64_t sys_secure_boot(int_frame_t* frame) {
    int cmd = (int)frame->rdi;
    unsigned long arg = frame->rsi;

    switch (cmd) {
    case 0: /* enable */
        secure_boot_set_enabled(arg ? 1 : 0);
        return 0;
    case 1: /* query */
        return secure_boot_is_enabled() ? 1 : 0;
    default:
        return (uint64_t)(int64_t)ERR_INVAL;
    }
}

/* ── Phase 1-6 gap syscalls ── */

static uint64_t sys_chmod(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    uint32_t mode = (uint32_t)frame->rsi;
    char kpath[256];
    if (copy_path_from_user(path, kpath, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)(int64_t)vfs_chmod(kpath, mode);
}

static uint64_t sys_link(int_frame_t* frame) {
    const char* target = (const char*)frame->rdi;
    const char* linkpath = (const char*)frame->rsi;
    char ktarget[256], klinkpath[256];
    if (copy_path_from_user(target, ktarget, sizeof(ktarget)) != 0 ||
        copy_path_from_user(linkpath, klinkpath, sizeof(klinkpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)(int64_t)vfs_link(ktarget, klinkpath);
}

static uint64_t sys_symlink(int_frame_t* frame) {
    const char* target = (const char*)frame->rdi;
    const char* linkpath = (const char*)frame->rsi;
    char ktarget[256], klinkpath[256];
    if (copy_path_from_user(target, ktarget, sizeof(ktarget)) != 0 ||
        copy_path_from_user(linkpath, klinkpath, sizeof(klinkpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)(int64_t)vfs_symlink(ktarget, klinkpath);
}

static uint64_t sys_readlink(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char* user_buf = (char*)frame->rsi;
    uint64_t size = frame->rdx;
    char kpath[256];
    if (copy_path_from_user(path, kpath, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    if (!user_buf || size == 0) return (uint64_t)(int64_t)ERR_INVAL;
    char kbuf[256];
    if (size > sizeof(kbuf)) size = sizeof(kbuf);
    int ret = vfs_readlink(kpath, kbuf, size);
    if (ret < 0) return (uint64_t)(int64_t)ret;
    if (copy_to_user(user_buf, kbuf, (uint32_t)ret) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)ret;
}

static uint64_t sys_rmdir(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    char kpath[256];
    if (copy_path_from_user(path, kpath, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)(int64_t)vfs_rmdir(kpath);
}

static uint64_t sys_ftruncate(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint64_t size = frame->rsi;
    return (uint64_t)(int64_t)vfs_ftruncate(fd, size);
}

static uint64_t sys_dup(int_frame_t* frame) {
    int oldfd = (int)frame->rdi;
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_fd_t* ft = vfs_get_fd_table();
    if (!ft[oldfd].used) return (uint64_t)(int64_t)ERR_BADFD;
    int newfd = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) { newfd = i; break; }
    }
    if (newfd < 0) return (uint64_t)(int64_t)ERR_NOSPACE;
    ft[newfd] = ft[oldfd];
    ft[newfd].offset = ft[oldfd].offset;
    if (ft[newfd].node)
        __sync_fetch_and_add(&ft[newfd].node->refcount, 1);
    return (uint64_t)newfd;
}

static uint64_t sys_access(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    int mode = (int)frame->rsi;
    char kpath[256];
    if (copy_path_from_user(path, kpath, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    /* F_OK (0): just check existence */
    vfs_node_t* node = vfs_find(kpath);
    if (!node) return (uint64_t)(int64_t)ERR_NOENT;
    if (mode == 0) return ERR_OK;

    /* R_OK (4), W_OK (2), X_OK (1) */
    int want_write = (mode & 2) ? 1 : 0;
    if (vfs_access_check(node, want_write) != 0)
        return (uint64_t)(int64_t)ERR_PERM;
    return ERR_OK;
}

static uint64_t sys_uname(int_frame_t* frame) {
    void* user_buf = (void*)frame->rdi;
    struct {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
    } uts;
    kmemset(&uts, 0, sizeof(uts));
    kstrncpy(uts.sysname, "OPERtur", sizeof(uts.sysname) - 1);
    kstrncpy(uts.nodename, "oper", sizeof(uts.nodename) - 1);
    kstrncpy(uts.release, "0.1.0", sizeof(uts.release) - 1);
    kstrncpy(uts.version, "TRY1", sizeof(uts.version) - 1);
    kstrncpy(uts.machine, "x86_64", sizeof(uts.machine) - 1);
    if (copy_to_user(user_buf, &uts, sizeof(uts)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return ERR_OK;
}

static uint64_t sys_nanosleep(int_frame_t* frame) {
    const struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } *user_req = (const void*)frame->rdi;
    void* user_rem = (void*)frame->rsi;
    struct ktimespec { int64_t sec; int64_t nsec; } kreq;
    if (copy_from_user(&kreq, user_req, sizeof(kreq)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    uint64_t ms = (uint64_t)kreq.sec * 1000ULL + (uint64_t)kreq.nsec / 1000000ULL;
    thread_sleep(ms);
    if (user_rem) {
        struct ktimespec remaining;
        kmemset(&remaining, 0, sizeof(remaining));
        if (copy_to_user(user_rem, &remaining, sizeof(remaining)) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
    }
    return ERR_OK;
}

static uint64_t sys_sync(int_frame_t* frame) {
    (void)frame;
    block_sync();
    return ERR_OK;
}

static uint64_t sys_fsync(int_frame_t* frame) {
    (void)frame;
    block_sync();
    return ERR_OK;
}

static uint64_t sys_fchmod(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint32_t mode = (uint32_t)frame->rsi;
    if (fd < 0 || fd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_fd_t* ft = vfs_get_fd_table();
    if (!ft[fd].used) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_node_t* node = ft[fd].node;
    if (!node) return (uint64_t)(int64_t)ERR_BADFD;
    err_t e = ERR_OK;
    if (node->fs && node->fs->ops && node->fs->ops->chmod)
        e = (err_t)node->fs->ops->chmod(node, mode);
    else
        e = ERR_NOSYS;
    return (uint64_t)(int64_t)e;
}

static uint64_t sys_fstat(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    vfs_stat_t* user_st = (vfs_stat_t*)frame->rsi;
    if (fd < 0 || fd >= VFS_MAX_FDS) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_fd_t* ft = vfs_get_fd_table();
    if (!ft[fd].used) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_node_t* node = ft[fd].node;
    if (!node) return (uint64_t)(int64_t)ERR_BADFD;
    vfs_stat_t kst;
    if (node->fs && node->fs->ops && node->fs->ops->stat) {
        int ret = node->fs->ops->stat(node, &kst);
        if (ret < 0) return (uint64_t)(int64_t)ERR_IO;
    } else {
        kmemset(&kst, 0, sizeof(kst));
        kst.size  = node->size;
        kst.inode = node->inode;
        kst.mode  = node->flags;
        kst.flags = node->flags;
    }
    if (copy_to_user(user_st, &kst, sizeof(kst)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return ERR_OK;
}

static uint64_t sys_mount(int_frame_t* frame) {
    (void)frame;
    return (uint64_t)(int64_t)ERR_NOSYS;
}

static uint64_t sys_umount(int_frame_t* frame) {
    (void)frame;
    return (uint64_t)(int64_t)ERR_NOSYS;
}

static uint64_t sys_lstat(int_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    vfs_stat_t* user_st = (vfs_stat_t*)frame->rsi;
    char kpath[256];
    if (copy_path_from_user(path, kpath, sizeof(kpath)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    vfs_node_t* node = vfs_find_nofollow(kpath);
    if (!node) return (uint64_t)(int64_t)ERR_NOENT;
    vfs_stat_t kst;
    if (node->fs && node->fs->ops && node->fs->ops->stat) {
        int ret = node->fs->ops->stat(node, &kst);
        if (ret < 0) return (uint64_t)(int64_t)ERR_IO;
    } else {
        kmemset(&kst, 0, sizeof(kst));
        kst.size  = node->size;
        kst.inode = node->inode;
        kst.mode  = node->flags;
        kst.flags = node->flags;
    }
    if (copy_to_user(user_st, &kst, sizeof(kst)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return ERR_OK;
}

static uint64_t sys_sched_setaffinity(int_frame_t* frame) {
    pid_t pid = (pid_t)frame->rdi;
    uint64_t mask;
    if (copy_from_user(&mask, (void*)frame->rsi, sizeof(mask)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;

    thread_t* t = NULL;
    if (pid == 0) {
        t = current_thread;
    } else {
        t = sched_find_thread_by_tid((uint64_t)pid);
    }
    if (!t)
        return (uint64_t)(int64_t)ERR_NOENT;

    sched_set_thread_affinity(t, mask);
    return ERR_OK;
}

typedef uint64_t (*syscall_fn)(int_frame_t*);
static syscall_fn syscall_table[] = {
    sys_exit,      /* 0 */
    sys_write,     /* 1 */
    sys_read,      /* 2 */
    sys_getpid,    /* 3 */
    sys_sbrk,      /* 4 */
    sys_open,      /* 5 */
    sys_close,     /* 6 */
    sys_readfile,  /* 7 */
    sys_writefile, /* 8 */
    sys_execve,    /* 9 */
    sys_fork,      /* 10 */
    sys_waitpid,   /* 11 */
    sys_getppid,   /* 12 */
    sys_yield,     /* 13 */
    sys_sleep,     /* 14 */
    sys_uptime,    /* 15 */
    sys_reboot,    /* 16 */
    sys_pwrdown,   /* 17 */
    sys_create,    /* 18 */
    sys_mkdir,     /* 19 */
    sys_unlink,    /* 20 */
    sys_lseek,     /* 21 */
    sys_stat,      /* 22 */
    sys_pipe,      /* 23 */
    sys_kill,      /* 24 */
    sys_sigaction, /* 25 */
    sys_clone,     /* 26 */
    sys_sigreturn, /* 27 */
    sys_getcwd,    /* 28 */
    sys_chdir,     /* 29 */
    sys_dup2,      /* 30 */
    sys_mmap,      /* 31 */
    sys_munmap,    /* 32 */
    sys_mprotect,  /* 33 */
    sys_ioctl,     /* 34 */
    sys_setpgid,   /* 35 */
    sys_getpgid,   /* 36 */
    sys_pty_pair,  /* 37 */
    sys_socket,    /* 38 */
    sys_bind,      /* 39 */
    sys_connect,   /* 40 */
    sys_listen,    /* 41 */
    sys_accept,    /* 42 */
    sys_send,      /* 43 */
    sys_recv,      /* 44 */
    sys_sendto,    /* 45 */
    sys_recvfrom,  /* 46 */
    sys_setsockopt,     /* 47 */
    sys_getsockopt,     /* 48 */
    sys_clock_gettime,  /* 49 */
    sys_getsockname,    /* 50 */
    sys_getpeername,    /* 51 */
    sys_poll,           /* 52 */
    sys_fcntl,          /* 53 */
    sys_capget,         /* 54 */
    sys_capset,         /* 55 */
    sys_audit_read,     /* 56 */
    sys_getuid,         /* 57 */
    sys_geteuid,        /* 58 */
    sys_getgid,         /* 59 */
    sys_getegid,        /* 60 */
    sys_setuid,         /* 61 */
    sys_setgid,         /* 62 */
    sys_getrandom,      /* 63 */
    sys_set_ssf,        /* 64 */
    sys_socketpair,     /* 65 */
    sys_prctl,          /* 66 */
    sys_secure_boot,    /* 67 */
    sys_unshare,        /* 68 */
    sys_veth_pair,      /* 69 */
    sys_netconfig,      /* 70 */
    sys_veth_move,      /* 71 */
    sys_chmod,          /* 72 */
    sys_link,           /* 73 */
    sys_symlink,        /* 74 */
    sys_readlink,       /* 75 */
    sys_rmdir,          /* 76 */
    sys_ftruncate,      /* 77 */
    sys_dup,            /* 78 */
    sys_access,         /* 79 */
    sys_uname,          /* 80 */
    sys_nanosleep,      /* 81 */
    sys_sync,           /* 82 */
    sys_fsync,          /* 83 */
    sys_fchmod,         /* 84 */
    sys_fstat,          /* 85 */
    sys_lstat,          /* 86 */
    sys_mount,          /* 87 */
    sys_umount,         /* 88 */
    sys_sched_setaffinity, /* 89 */
};

void syscall_init(void) {
    kprintf("[SYSCALL] int 0x80 handler registered, %lu syscalls\n", SYSCALL_COUNT);
}

void syscall_handler(int_frame_t* frame) {
    uint64_t num = frame->rax;
    if (num >= SYSCALL_COUNT) {
        frame->rax = (uint64_t)(int64_t)(-ENOSYS);
        return;
    }
    /* Per-process syscall filter check */
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (proc) {
        int idx = num / 64;
        int bit = num % 64;
        if (!(proc->syscall_mask[idx] & (1ULL << bit))) {
            frame->rax = (uint64_t)(int64_t)(-EPERM);
            return;
        }
    }
    int64_t ret = (int64_t)syscall_table[num](frame);
    /* Translate kernel ERR_* to POSIX errno for userspace.
     * Positive return values (fds, byte counts) pass through unchanged. */
    if (ret < 0) {
        int perr = kernel_err_to_posix((int)(-ret));
        ret = -(int64_t)perr;
    }
    frame->rax = (uint64_t)ret;
}
