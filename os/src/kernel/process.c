#include "process.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "sync.h"
#include "elf.h"
#include "hal.h"
#include "vfs.h"
#include "kmalloc.h"

/* ASLR helpers */
#define ASLR_STACK_PAGES 0x100
#define ASLR_STACK_BASE  0x60000000ULL

static uint64_t aslr_rand(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t rdtsc_val = ((uint64_t)hi << 32) | lo;
    uint64_t ticks = hal_timer_get_ticks();
    return rdtsc_val ^ (ticks * 6364136223846793005ULL + 1442695040888963407ULL);
}

static process_t process_table[MAX_PROCESSES];
static pid_t next_pid = 1;
static list_head_t process_list;
static spinlock_t process_lock;
static uint64_t kernel_cr3 = 0;

err_t process_init(void) {
    kmemset(process_table, 0, sizeof(process_table));
    list_init(&process_list);
    spinlock_init(&process_lock, "process_lock");

    /* Save kernel CR3 (identity mapping available) for PMM ops
     * that need to access physical addresses directly */
    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));

    // Create init process (pid 1)
    process_t* init_proc = process_create("init", 0);
    if (!init_proc) {
        return ERR_NOMEM;
    }

    kprintf("[PROCESS] Process manager initialized, init pid = %d\n", init_proc->pid);
    return ERR_OK;
}

process_t* process_create(const char* name, pid_t ppid) {
    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);

    // Find free slot in process table
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        spinlock_release(&process_lock, _sflags);
        return NULL;
    }

    process_t* proc = &process_table[slot];
    kmemset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->ppid = ppid;
    proc->pgid = proc->pid;
    kstrncpy(proc->name, name, PROCESS_NAME_MAX - 1);
    list_init(&proc->threads);
    proc->pending_signals = 0;
    proc->blocked_signals = 0;
    proc->flags = 0;
    kmemset(proc->signal_actions, 0, sizeof(proc->signal_actions));
    spinlock_init(&proc->signal_lock, "signal_lock");
    proc->cwd[0] = '/';
    proc->cwd[1] = 0;

    // Allocate new page table (copy kernel mappings)
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        spinlock_release(&process_lock, _sflags);
        return NULL;
    }

    // Zero new PML4
    kmemset((void*)PHYS_TO_VIRT(pml4_phys), 0, PAGE_SIZE);

    // Copy kernel mappings (top half) from current CR3
    cpu_flags_t irq_flags = hal_save_irq();
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    uint64_t* current_pml4 = (uint64_t*)PHYS_TO_VIRT(cr3_val);
    uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = current_pml4[i];
    }
    hal_restore_irq(irq_flags);

    /* Stamp PML4 with pid so process_exit can detect stale reuse */
    new_pml4[255] = (uint64_t)proc->pid;

    proc->cr3 = pml4_phys;
    proc->mmap_brk = 0x40000000;

    list_add_tail(&process_list, &proc->process_node);
    spinlock_release(&process_lock, _sflags);

    /* Inherit fd table from parent process (lock released, use process_find) */
    if (ppid > 0) {
        process_t* parent = process_find(ppid);
        if (parent) {
            kmemcpy(proc->fds, parent->fds, sizeof(proc->fds));
            /* Bump refcount on every inherited fd so the shared vfs_node_t
             * does not get freed when the parent (or child) closes its copy. */
            for (int i = 0; i < MAX_FDS; i++) {
                if (proc->fds[i].used && proc->fds[i].node)
                    __sync_fetch_and_add(&proc->fds[i].node->refcount, 1);
            }
        }
    }

    return proc;
}

err_t process_exec(process_t* proc, const void* elf_data, size_t elf_len) {
    /* Step 1: Scan for PT_INTERP in the ELF */
    char interp_path[256] = {0};
    int has_interp = 0;
    {
        const elf64_hdr_t* hdr = (const elf64_hdr_t*)elf_data;
        if (elf_len >= sizeof(elf64_hdr_t) && hdr->magic == ELF_MAGIC && hdr->cls == ELF_64) {
            if (hdr->phoff + (uint64_t)hdr->phnum * hdr->phentsize > elf_len) return ERR_INVAL;
            const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + hdr->phoff);
            for (uint16_t i = 0; i < hdr->phnum; i++) {
                if (ph[i].type == PT_INTERP) {
                    size_t plen = ph[i].filesz;
                    if (plen > sizeof(interp_path) - 1) plen = sizeof(interp_path) - 1;
                    kmemcpy(interp_path, (const void*)((uint64_t)elf_data + ph[i].offset), plen);
                    interp_path[plen] = '\0';
                    /* Trim trailing newline if present */
                    while (plen > 0 && (interp_path[plen-1] == '\n' || interp_path[plen-1] == '\r'))
                        interp_path[--plen] = '\0';
                    has_interp = 1;
                    break;
                }
            }
        }
    }

    /* Step 2: Load main program */
    err_t e = elf_load(proc, elf_data, elf_len);
    if (e) return e;

    /* Save main program info for aux vector */
    const elf64_hdr_t* mhdr = (const elf64_hdr_t*)elf_data;
    uint64_t main_entry  = proc->entry_point;
    uint64_t main_base   = main_entry - mhdr->entry;
    uint64_t main_phdr   = elf_phdr_vaddr(elf_data, main_base);
    uint16_t main_phnum  = mhdr->phnum;

    /* Step 3: Load interpreter if needed */
    uint64_t interp_entry = 0;
    uint64_t interp_base  = 0;

    if (has_interp) {
        int fd = vfs_open(interp_path, 0);
        if (fd < 0) {
            kprintf("[PROCESS] Interpreter '%s' not found\n", interp_path);
            return ERR_NOENT;
        }
        uint64_t fsz = vfs_lseek(fd, 0, VFS_SEEK_END);
        vfs_lseek(fd, 0, VFS_SEEK_SET);
        uint8_t* ibuf = (uint8_t*)kmalloc(fsz);
        if (!ibuf) { vfs_close(fd); return ERR_NOMEM; }
        vfs_read(fd, ibuf, fsz);
        vfs_close(fd);

        /* Load interpreter at fixed address to avoid conflict with main program */
        const elf64_hdr_t* ihdr = (const elf64_hdr_t*)ibuf;
        uint64_t interp_load_at = 0x7F000000;
        e = elf_load_fixed(proc, ibuf, fsz, interp_load_at);
        if (e) { kfree(ibuf); return e; }

        interp_entry = proc->entry_point;
        interp_base  = interp_entry - ihdr->entry;
        kfree(ibuf);

        /* Verify the interpreter was loaded at the expected base */
        if (interp_base != interp_load_at && ihdr->type != ELF_EXEC)
            interp_base = interp_load_at; /* for EXEC the base is the linked address itself */
    }

    /* Step 4: Set heap base (brk) to just after the ELF's last mapped page */
    {
        uint64_t max_addr = 0x40000000; /* safe fallback */
        const elf64_hdr_t* eh = (const elf64_hdr_t*)elf_data;
        if (elf_len >= sizeof(elf64_hdr_t) && eh->magic == ELF_MAGIC) {
            uint64_t base_offset = 0;
            if (eh->type == ELF_DYN)
                base_offset = 0x40000000; /* conservative: DYN loads at 0x40000000..0x60000000 */
            const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + eh->phoff);
            for (uint16_t i = 0; i < eh->phnum; i++) {
                if (ph[i].type == PT_LOAD) {
                    uint64_t end = ph[i].vaddr + base_offset + ph[i].memsz;
                    if (end > max_addr) max_addr = end;
                }
            }
        }
        /* Page-align and leave a 64KB gap */
        proc->mmap_brk = (max_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
        if (proc->mmap_brk <= max_addr) proc->mmap_brk = max_addr + 0x10000;
    }

    /* Step 5: Set up user stack */
    uint64_t stack_page = pmm_alloc_page();
    if (!stack_page) return ERR_NOMEM;
    kmemset((void*)PHYS_TO_VIRT(stack_page), 0, PAGE_SIZE);

    uint64_t stack_offset = (aslr_rand() & (ASLR_STACK_PAGES - 1)) * PAGE_SIZE;
    uint64_t user_stack   = ASLR_STACK_BASE + stack_offset;
    uint64_t user_stack_top = user_stack + PAGE_SIZE;

    uint64_t cr3 = proc->cr3;
    vmm_map_page(cr3, user_stack, stack_page, PAGE_USER | PAGE_WRITE);

    /* Build ELF ABI stack layout */
    uint8_t* stk = (uint8_t*)PHYS_TO_VIRT(stack_page);

    size_t nlen = kstrlen(proc->name) + 1;
    uint64_t string_off = PAGE_SIZE - nlen;
    kmemcpy(stk + string_off, proc->name, nlen);
    string_off &= ~7ULL;
    uint64_t prog_vaddr = user_stack + string_off;

    uint64_t n_auxv = has_interp ? 14 : 0;
    uint64_t abi_size = 32 + n_auxv * 8;

    uint64_t rsp_off = string_off - abi_size;
    rsp_off &= ~(16ULL - 1);
    user_stack_top = user_stack + rsp_off;

    uint64_t pos = rsp_off;
    *(uint64_t*)(stk + pos) = 1;          pos += 8;
    *(uint64_t*)(stk + pos) = prog_vaddr; pos += 8;
    *(uint64_t*)(stk + pos) = 0;          pos += 8;
    *(uint64_t*)(stk + pos) = 0;          pos += 8;

    if (has_interp) {
        *(uint64_t*)(stk + pos) = 3;          pos += 8;
        *(uint64_t*)(stk + pos) = main_phdr;  pos += 8;
        *(uint64_t*)(stk + pos) = 4;                     pos += 8;
        *(uint64_t*)(stk + pos) = sizeof(elf64_phdr_t);  pos += 8;
        *(uint64_t*)(stk + pos) = 5;           pos += 8;
        *(uint64_t*)(stk + pos) = main_phnum;  pos += 8;
        *(uint64_t*)(stk + pos) = 6;      pos += 8;
        *(uint64_t*)(stk + pos) = 4096;   pos += 8;
        *(uint64_t*)(stk + pos) = 7;           pos += 8;
        *(uint64_t*)(stk + pos) = interp_base; pos += 8;
        *(uint64_t*)(stk + pos) = 9;          pos += 8;
        *(uint64_t*)(stk + pos) = main_entry; pos += 8;
        *(uint64_t*)(stk + pos) = 0; pos += 8;
        *(uint64_t*)(stk + pos) = 0; pos += 8;
    }

    /* Update entry point to interpreter if dynamic linking */
    if (has_interp) {
        proc->entry_point = interp_entry;
    }

    /* Set heap base (brk) immediately after the user stack */
    proc->user_stack_top = user_stack + PAGE_SIZE;

    /* Map signal trampoline page for sigreturn, plus auxv data at a fixed offset */
    {
        uint64_t tramp_page = pmm_alloc_page();
        if (!tramp_page) return ERR_NOMEM;
        kmemset((void*)PHYS_TO_VIRT(tramp_page), 0, PAGE_SIZE);
        uint8_t* tramp = (uint8_t*)PHYS_TO_VIRT(tramp_page);
        tramp[0] = 0x48; tramp[1] = 0x89; tramp[2] = 0xE7;
        tramp[3] = 0x48; tramp[4] = 0xC7; tramp[5] = 0xC0;
        tramp[6] = 27;   tramp[7] = 0x00; tramp[8] = 0x00; tramp[9] = 0x00;
        tramp[10] = 0xCD; tramp[11] = 0x80;
        tramp[12] = 0xEB; tramp[13] = 0xFD;
        vmm_map_page(cr3, SIGNAL_TRAMPOLINE_ADDR, tramp_page,
                     PAGE_PRESENT | PAGE_USER);

        /* Write auxv + argc at fixed offset 0x100 in the trampoline page */
        uint64_t* ap = (uint64_t*)(tramp + 0x100);
        if (has_interp) {
            *ap++ = 3;    *ap++ = main_phdr;
            *ap++ = 4;    *ap++ = sizeof(elf64_phdr_t);
            *ap++ = 5;    *ap++ = main_phnum;
            *ap++ = 6;    *ap++ = 4096;
            *ap++ = 7;    *ap++ = interp_base;
            *ap++ = 9;    *ap++ = main_entry;
            *ap++ = 0;    *ap++ = 0;
        } else {
            *ap++ = 0; *ap++ = 0; /* AT_NULL sentinel */
        }
        /* Store argc and argv[0] at fixed offsets for ld.so */
        *(uint64_t*)(tramp + 0x0F0) = 1;
        *(uint64_t*)(tramp + 0x0F8) = prog_vaddr;
    }

    thread_t* tcb = (thread_t*)PHYS_TO_VIRT(pmm_alloc_page());
    if (!tcb) return ERR_NOMEM;
    kmemset(tcb, 0, sizeof(thread_t));

    uint64_t kstack_phys = pmm_alloc_pages(
        (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!kstack_phys) {
        pmm_free_page((uint64_t)tcb - KERNEL_VMA_BASE);
        return ERR_NOMEM;
    }
    void* kstack = (void*)PHYS_TO_VIRT(kstack_phys);
    kmemset(kstack, 0, THREAD_STACK_SIZE);
    uint64_t kstack_top = (uint64_t)kstack + THREAD_STACK_SIZE;

    uint64_t* sp = (uint64_t*)kstack_top;

    *(--sp) = USER_DS;
    *(--sp) = user_stack_top;
    *(--sp) = 0x202;
    *(--sp) = USER_CS;
    *(--sp) = proc->entry_point;

    *(--sp) = 0;
    *(--sp) = 0;

    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    *(--sp) = (uint64_t)user_thread_entry;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    tcb->id = 0;
    tcb->rsp = (uint64_t)sp;
    tcb->cr3 = cr3;
    tcb->state = THREAD_CREATED;
    tcb->priority = THREAD_DEF_PRIO;
    tcb->base_priority = THREAD_DEF_PRIO;
    tcb->time_slice_remaining = 0;
    tcb->kernel_stack = kstack;
    tcb->kernel_stack_size = THREAD_STACK_SIZE;
    tcb->user_code_page = 0;
    tcb->user_stack_page = stack_page;
    tcb->join_queue.waiters = NULL;
    tcb->join_queue.count = 0;
    tcb->proc = proc;
    kstrncpy(tcb->name, proc->name, THREAD_NAME_MAX - 1);

    extern void all_threads_add(thread_t* t);
    all_threads_add(tcb);

    list_add_tail(&proc->threads, &tcb->threads_node);
    proc->thread_count++;

    sched_add_thread(tcb);

    return ERR_OK;
}

err_t process_exit(process_t* proc, int exit_code) {
    if (!proc) return ERR_INVAL;

    proc->exit_code = exit_code;
    proc->exited = true;

    /* Send SIGCHLD to parent */
    if (proc->ppid > 0) {
        process_t* parent = process_find(proc->ppid);
        if (parent) {
            signal_send(proc->ppid, SIGCHLD);
            signal_process(parent);
        }
    }

    /* Reparent orphan children to init (pid 1) */
    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid > 0 && process_table[i].ppid == proc->pid) {
            process_table[i].ppid = 1;
        }
    }
    spinlock_release(&process_lock, _sflags);

    /* Close all open file descriptors via vfs_close (uses refcounting) */
    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fds[i].used) {
            vfs_close(i);
        }
    }

    /* Wake any waitpid waiters */
    sched_wake(&proc->exit_waiters);

    /* Free resources: user pages and slot.  If we are the last thread
     * in the process (thread_count <= 0) then free everything.
     * NOTE: the slot (proc->pid) stays valid for waitpid to collect
     * the exit status — it is cleared by process_reap() instead. */
    if (proc->cr3 && proc->thread_count <= 0) {
        /* Save kernel CR3 on first call */
        if (!kernel_cr3) {
            uint64_t cr3_val;
            asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
            kernel_cr3 = cr3_val;
        }

        /* Switch to kernel page table for PMM operations */
        uint64_t saved_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(saved_cr3));
        if (saved_cr3 != kernel_cr3)
            asm volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

        /* Validate PML4 belongs to this process before freeing */
        {
            page_entry_t* pml4v = (page_entry_t*)PHYS_TO_VIRT(proc->cr3);
            uint64_t stamp = pml4v[255];
            if (stamp != (uint64_t)proc->pid) {
                kprintf("[EXIT] WARN pid %d (%s) PML4=0x%lx stamp=%lu != pid=%d — "
                        "page reused, skipping free\n",
                        proc->pid, proc->name, proc->cr3, stamp, proc->pid);
                proc->cr3 = 0;
                if (saved_cr3 != kernel_cr3)
                    asm volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
                return ERR_OK;
            }
            /* Clear PID stamp so vmm_free_user_pages doesn't misinterpret
             * pml4[255] as a present page-table entry and try to read
             * physical page 0 (BIOS IVT) as a PDPT */
            pml4v[255] = 0;
        }

        vmm_free_user_pages(proc->cr3);
        pmm_free_page(proc->cr3);
        proc->cr3 = 0;

        /* Stay on kernel CR3 -- do NOT restore saved_cr3 if it was
         * the process's now-freed PML4 (e.g., clone threads sharing CR3) */
    }

    return ERR_OK;
}

/* Reap a zombie process: free the pid slot.  Called from sys_waitpid
 * after the exit status has been collected.  The child's pages have
 * already been freed by process_exit, so this just clears the pid. */
void process_reap(process_t* proc) {
    if (!proc) return;
    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);
    proc->process_node.prev->next = proc->process_node.next;
    proc->process_node.next->prev = proc->process_node.prev;
    list_init(&proc->process_node);
    proc->pid = 0;  /* frees the slot in process_table */
    spinlock_release(&process_lock, _sflags);
}

process_t* process_find(pid_t pid) {
    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid == pid) {
            spinlock_release(&process_lock, _sflags);
            return &process_table[i];
        }
    }
    spinlock_release(&process_lock, _sflags);
    return NULL;
}

pid_t process_get_current_pid(void) {
    if (current_thread && current_thread->proc)
        return current_thread->proc->pid;
    return 0;
}

static int signal_default_action(int sig) {
    switch (sig) {
        case SIGKILL:
        case SIGTERM:
        case SIGINT:
        case SIGQUIT:
        case SIGILL:
        case SIGABRT:
        case SIGFPE:
        case SIGSEGV:
        case SIGPIPE:
        case SIGALRM:
        case SIGBUS:
        case SIGSYS:
        case SIGXCPU:
        case SIGXFSZ:
            return SIGACT_TERM;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return SIGACT_STOP;
        case SIGCONT:
            return SIGACT_CONT;
        default:
            return SIGACT_IGN;
    }
}

void signal_send(pid_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) return;
    process_t* proc = process_find(pid);
    if (!proc) return;

    cpu_flags_t _sflags; spinlock_acquire(&proc->signal_lock, &_sflags);
    /* SIGCONT cancels pending stop signals */
    if (sig == SIGCONT)
        proc->pending_signals &= ~((1UL << SIGSTOP) | (1UL << SIGTSTP));

    /* SIGSTOP/SIGTSTP cancel pending SIGCONT */
    if (sig == SIGSTOP || sig == SIGTSTP)
        proc->pending_signals &= ~(1UL << SIGCONT);

    proc->pending_signals |= (1UL << sig);
    spinlock_release(&proc->signal_lock, _sflags);
}

void signal_process(process_t* proc) {
    if (!proc) return;
    uint64_t pending = proc->pending_signals & ~proc->blocked_signals;
    if (!pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1UL << sig))) continue;
        proc->pending_signals &= ~(1UL << sig);

        /* Check for custom handler */
        if (proc->signal_actions[sig].sa_handler &&
            proc->signal_actions[sig].sa_handler != SIG_DFL &&
            proc->signal_actions[sig].sa_handler != SIG_IGN) {
            /* Custom handler delivery happens in interrupt_handler
             * where the int_frame_t is available. Skip here. */
            continue;
        }

        /* Check for ignore */
        if (proc->signal_actions[sig].sa_handler == SIG_IGN)
            continue;

        /* Default action */
        switch (signal_default_action(sig)) {
            case SIGACT_TERM: {
                kprintf("[SIGNAL] pid %d TERMINATED by signal %d (exit_code=%d)\n", proc->pid, sig, 128 + sig);
                process_exit(proc, 128 + sig);
                return;
            }
            case SIGACT_STOP: {
                if (!(proc->flags & PROC_FLAG_STOPPED)) {
                    proc->flags |= PROC_FLAG_STOPPED;
                    kprintf("[SIGNAL] pid %d stopped by signal %d\n", proc->pid, sig);
                    /* Block all threads in the process */
                    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);
                    struct list_head* iter = proc->threads.next;
                    while (iter != &proc->threads) {
                        thread_t* t = container_of(iter, thread_t, threads_node);
                        iter = iter->next;
                        if (t->state == THREAD_READY || t->state == THREAD_RUNNING) {
                            sched_remove_thread(t);
                            t->state = THREAD_BLOCKED;
                        }
                    }
                    spinlock_release(&process_lock, _sflags);
                }
                return;
            }
            case SIGACT_CONT: {
                if (proc->flags & PROC_FLAG_STOPPED) {
                    proc->flags &= ~PROC_FLAG_STOPPED;
                    kprintf("[SIGNAL] pid %d continued by signal %d\n", proc->pid, sig);
                    /* Unblock all threads in the process */
                    struct list_head* iter = proc->threads.next;
                    while (iter != &proc->threads) {
                        thread_t* t = container_of(iter, thread_t, threads_node);
                        iter = iter->next;
                        if (t->state == THREAD_BLOCKED) {
                            sched_add_thread(t);
                        }
                    }
                }
                return;
            }
            default:
                break;
        }
    }
}

void signal_send_pgid(pid_t pgid, int sig) {
    if (sig < 0 || sig >= NSIG) return;
    if (pgid <= 0) return;

    pid_t pids[MAX_PROCESSES];
    int count = 0;

    cpu_flags_t _sflags; spinlock_acquire(&process_lock, &_sflags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid > 0 && process_table[i].pgid == pgid) {
            pids[count++] = process_table[i].pid;
        }
    }
    spinlock_release(&process_lock, _sflags);

    for (int i = 0; i < count; i++) {
        signal_send(pids[i], sig);
        process_t* p = process_find(pids[i]);
        signal_process(p);
    }
}

void signal_deliver_custom(process_t* proc, int_frame_t* frame) {
    if (!proc || !frame) return;
    if ((frame->cs & 3) != 3) return; /* not from user mode */

    cpu_flags_t _sflags; spinlock_acquire(&proc->signal_lock, &_sflags);
    uint64_t pending = proc->pending_signals & ~proc->blocked_signals;
    if (!pending) { spinlock_release(&proc->signal_lock, _sflags); return; }

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1UL << sig))) continue;

        void* handler = proc->signal_actions[sig].sa_handler;
        if (!handler || handler == SIG_DFL || handler == SIG_IGN)
            continue;

        proc->pending_signals &= ~(1UL << sig);
        spinlock_release(&proc->signal_lock, _sflags);

        /* Build sigframe from saved user context */
        sigframe_t sf;
        sf.rax  = frame->rax;
        sf.rbx  = frame->rbx;
        sf.rcx  = frame->rcx;
        sf.rdx  = frame->rdx;
        sf.rsi  = frame->rsi;
        sf.rdi  = frame->rdi;
        sf.rbp  = frame->rbp;
        sf.r8   = frame->r8;
        sf.r9   = frame->r9;
        sf.r10  = frame->r10;
        sf.r11  = frame->r11;
        sf.r12  = frame->r12;
        sf.r13  = frame->r13;
        sf.r14  = frame->r14;
        sf.r15  = frame->r15;
        sf.rip  = frame->rip;
        sf.cs   = frame->cs;
        sf.rflags = frame->rflags;
        sf.rsp  = frame->rsp;
        sf.ss   = frame->ss;
        sf.sig  = sig;
        sf.pad  = 0;

        /* Push onto user stack (low to high):
         *   [return address = SIGNAL_TRAMPOLINE_ADDR]  <- handler's RSP
         *   [sigframe_t]                                 <- rdi for sigreturn
         */
        uint64_t user_rsp = frame->rsp;
        user_rsp -= sizeof(sigframe_t);
        user_rsp &= ~15ULL;
        user_rsp -= 8;

        {
            uint64_t tramp_addr = SIGNAL_TRAMPOLINE_ADDR;
            if (copy_to_user((void*)user_rsp, &tramp_addr, 8) != 0)
                return;
        }
        if (copy_to_user((void*)(user_rsp + 8), &sf, sizeof(sf)) != 0)
            return;

        /* Modify int_frame to invoke user handler */
        frame->rdi = sig;
        frame->rip = (uint64_t)handler;
        frame->rsp = user_rsp;
        return;
    }
    spinlock_release(&proc->signal_lock, _sflags);
}
