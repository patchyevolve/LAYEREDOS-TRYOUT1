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

#define USER_VIRT_START 0x40000000UL
#define USER_VIRT_END   0x80000000UL

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
    if (!is_user_range_valid((uint64_t)src, n)) return ERR_FAULT;
    if (n == 0) return 0;
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
    if (!is_user_range_valid((uint64_t)dst, n)) return ERR_FAULT;
    if (n == 0) return 0;
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
        proc->thread_count--;
        if (proc->thread_count <= 0 && !proc->exited) {
            process_exit(proc, exit_code);
        } else {
            proc->exit_code = exit_code;
            proc->exited = true;
            sched_wake(&proc->exit_waiters);
        }
    }
    thread_exit(exit_code);
    return 0;
}

static uint64_t sys_write(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    const char* buf = (const char*)frame->rsi;
    size_t count = (size_t)frame->rdx;

    if (fd <= 2) {
        char kbuf[256];
        size_t written = 0;
        while (count > 0) {
            size_t chunk = count;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, buf + written, chunk) != 0)
                return (uint64_t)(int64_t)ERR_FAULT;
            for (size_t i = 0; i < chunk; i++)
                kputchar(kbuf[i]);
            written += chunk;
            count -= chunk;
        }
        return (uint64_t)written;
    }
    return (uint64_t)(int64_t)ERR_NOSYS;
}

static uint64_t sys_read(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    char* buf = (char*)frame->rsi;
    size_t count = (size_t)frame->rdx;

    if (fd <= 2) {
        char kbuf[256];
        size_t i;
        for (i = 0; i < count && i < sizeof(kbuf); i++) {
            char c = hal_uart_getchar();
            kbuf[i] = c;
            if (c == '\n' || c == '\r') { i++; break; }
        }
        if (copy_to_user(buf, kbuf, i) != 0)
            return (uint64_t)(int64_t)ERR_FAULT;
        return i;
    }
    return (uint64_t)(int64_t)ERR_NOSYS;
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
        uint64_t new_brk = old_brk + increment;
        uint64_t pages_needed = (new_brk - proc->user_stack_top + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t cr3_val;
        asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
        for (uint64_t i = 0; i < pages_needed; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) return (uint64_t)-1;
            kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            vmm_map_page(cr3_val, old_brk + i * PAGE_SIZE, phys, PAGE_USER | PAGE_WRITE);
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
    if (vfs_close(fd) < 0)
        return (uint64_t)(int64_t)ERR_BADFD;
    return 0;
}

static uint64_t sys_readfile(int_frame_t* frame) {
    int fd = (int)frame->rdi;
    char* buf = (char*)frame->rsi;
    size_t count = (size_t)frame->rdx;
    char kbuf[512];
    size_t chunk = count;
    if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
    int64_t ret = vfs_read(fd, kbuf, chunk);
    if (ret < 0) return (uint64_t)(int64_t)ERR_IO;
    if (copy_to_user(buf, kbuf, (size_t)ret) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
    return (uint64_t)ret;
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
    err_t e = elf_load(proc, buf, sz);
    kfree(buf);
    if (e) return (uint64_t)(int64_t)e;
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    if (proc->cr3 && proc->cr3 != old_cr3)
        asm volatile("mov %0, %%cr3" : : "r"(proc->cr3) : "memory");
    uint64_t stack_page = pmm_alloc_page();
    if (!stack_page) return (uint64_t)(int64_t)ERR_NOMEM;
    kmemset((void*)PHYS_TO_VIRT(stack_page), 0, PAGE_SIZE);
    uint64_t user_stack = 0x70000000;
    uint64_t user_stack_top = user_stack + PAGE_SIZE - 8;
    vmm_map_page(proc->cr3, user_stack, stack_page, PAGE_USER | PAGE_WRITE);
    if (proc->cr3 != old_cr3)
        asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    frame->rip = proc->entry_point;
    frame->rsp = user_stack_top;
    frame->rdi = 0;
    frame->rsi = 0;
    return 0;
}

static uint64_t sys_fork(int_frame_t* frame) {
    thread_t* parent = current_thread;
    process_t* pp = parent ? parent->proc : NULL;
    if (!pp) return (uint64_t)(int64_t)ERR_INVAL;
    process_t* cp = process_create(pp->name, pp->pid);
    if (!cp) return (uint64_t)(int64_t)ERR_NOMEM;
    if (pp->cr3 && cp->cr3) {
        err_t e = vmm_duplicate_user_pages(cp->cr3, pp->cr3);
        if (e) return (uint64_t)(int64_t)e;
    }
    cp->entry_point = pp->entry_point;
    cp->user_stack_top = pp->user_stack_top;
    cp->user_code_start = pp->user_code_start;
    cp->user_code_size = pp->user_code_size;
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
    ct->time_slice_remaining = 0;
    ct->kernel_stack = ks;
    ct->kernel_stack_size = THREAD_STACK_SIZE;
    ct->proc = cp;
    kstrncpy(ct->name, pp->name, THREAD_NAME_MAX - 1);
    all_threads_add(ct);
    list_add_tail(&cp->threads, &ct->threads_node);
    cp->thread_count++;
    sched_add_thread(ct);
    return cp->pid;
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

    /* Same stack layout as fork (see sys_fork for details) */
    *(--sp) = frame->ss;
    *(--sp) = child_stack_user ? child_stack_user : frame->rsp;
    *(--sp) = frame->rflags;
    *(--sp) = frame->cs;
    *(--sp) = frame->rip;

    *(--sp) = frame->error_code;
    *(--sp) = frame->vector;

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

    *(--sp) = (uint64_t)fork_child_entry;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    ct->rsp = (uint64_t)sp;
    ct->cr3 = proc->cr3; /* Same address space */
    ct->state = THREAD_CREATED;
    ct->priority = parent->priority;
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
    hal_reboot();
    return 0;
}

static uint64_t sys_pwrdown(int_frame_t* frame) {
    (void)frame;
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
    if (copy_to_user(fds, kfds, sizeof(kfds)) != 0)
        return (uint64_t)(int64_t)ERR_FAULT;
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
};

void syscall_init(void) {
    kprintf("[SYSCALL] int 0x80 handler registered, %lu syscalls\n", SYSCALL_COUNT);
}

void syscall_handler(int_frame_t* frame) {
    uint64_t num = frame->rax;
    if (num >= SYSCALL_COUNT) {
        frame->rax = (uint64_t)(int64_t)ERR_NOSYS;
        return;
    }
    frame->rax = syscall_table[num](frame);
}
