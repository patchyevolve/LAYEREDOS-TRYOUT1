#include "process.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "sync.h"
#include "elf.h"
#include "hal.h"

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
    spinlock_acquire(&process_lock);

    // Find free slot in process table
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        spinlock_release(&process_lock);
        return NULL;
    }

    process_t* proc = &process_table[slot];
    kmemset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->ppid = ppid;
    kstrncpy(proc->name, name, PROCESS_NAME_MAX - 1);
    list_init(&proc->threads);
    proc->pending_signals = 0;
    proc->blocked_signals = 0;
    proc->flags = 0;
    kmemset(proc->signal_actions, 0, sizeof(proc->signal_actions));

    // Allocate new page table (copy kernel mappings)
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        spinlock_release(&process_lock);
        return NULL;
    }

    // Zero new PML4
    kmemset((void*)PHYS_TO_VIRT(pml4_phys), 0, PAGE_SIZE);

    // Copy kernel mappings (top half) from current CR3
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    uint64_t* current_pml4 = (uint64_t*)PHYS_TO_VIRT(cr3_val);
    uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = current_pml4[i];
    }

    proc->cr3 = pml4_phys;

    list_add_tail(&process_list, &proc->process_node);
    spinlock_release(&process_lock);

    return proc;
}

err_t process_exec(process_t* proc, const void* elf_data, size_t elf_len) {
    err_t e = elf_load(proc, elf_data, elf_len);
    if (e) return e;

    uint64_t stack_page = pmm_alloc_page();
    if (!stack_page) return ERR_NOMEM;
    kmemset((void*)PHYS_TO_VIRT(stack_page), 0, PAGE_SIZE);

    /* Randomize stack address */
    uint64_t stack_offset = (aslr_rand() % ASLR_STACK_PAGES) * PAGE_SIZE;
    uint64_t user_stack   = ASLR_STACK_BASE + stack_offset;
    uint64_t user_stack_top = user_stack + PAGE_SIZE;

    uint64_t cr3 = proc->cr3;
    vmm_map_page(cr3, user_stack, stack_page, PAGE_USER | PAGE_WRITE);

    /* Set heap base (brk) below stack */
    proc->user_stack_top = user_stack;

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

    for (int i = 0; i < 15; i++) *(--sp) = 0;

    *(--sp) = (uint64_t)user_thread_entry;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    tcb->id = 0;
    tcb->rsp = (uint64_t)sp;
    tcb->cr3 = cr3;
    tcb->state = THREAD_CREATED;
    tcb->priority = THREAD_DEF_PRIO;
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

    kprintf("[PROCESS] Exec'd %s (pid %d, entry=%llx, stack=%llx)\n",
            proc->name, proc->pid, proc->entry_point, user_stack);
    return ERR_OK;
}

err_t process_exit(process_t* proc, int exit_code) {
    if (!proc) return ERR_INVAL;

    proc->exit_code = exit_code;
    proc->exited = true;

    /* Send SIGCHLD to parent */
    if (proc->ppid > 0) {
        signal_send(proc->ppid, SIGCHLD);
        signal_process(process_find(proc->ppid));
    }

    /* Wake any waitpid waiters */
    sched_wake(&proc->exit_waiters);

    /* Free all user pages and page tables */
    if (proc->cr3) {
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

        vmm_free_user_pages(proc->cr3);
        pmm_free_page(proc->cr3);
        proc->cr3 = 0;

        /* Stay on kernel CR3 -- do NOT restore saved_cr3 if it was
         * the process's now-freed PML4 (e.g., clone threads sharing CR3) */
    }

    /* Remove from process list and recycle slot */
    spinlock_acquire(&process_lock);
    proc->process_node.prev->next = proc->process_node.next;
    proc->process_node.next->prev = proc->process_node.prev;
    list_init(&proc->process_node);
    proc->pid = 0;  /* frees the slot in process_table */
    spinlock_release(&process_lock);

    return ERR_OK;
}

process_t* process_find(pid_t pid) {
    spinlock_acquire(&process_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].pid == pid) {
            spinlock_release(&process_lock);
            return &process_table[i];
        }
    }
    spinlock_release(&process_lock);
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

    /* SIGCONT cancels pending stop signals */
    if (sig == SIGCONT)
        proc->pending_signals &= ~((1UL << SIGSTOP) | (1UL << SIGTSTP));

    /* SIGSTOP/SIGTSTP cancel pending SIGCONT */
    if (sig == SIGSTOP || sig == SIGTSTP)
        proc->pending_signals &= ~(1UL << SIGCONT);

    proc->pending_signals |= (1UL << sig);
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
            /* Custom handler would be delivered here. Skip for now. */
            continue;
        }

        /* Check for ignore */
        if (proc->signal_actions[sig].sa_handler == SIG_IGN)
            continue;

        /* Default action */
        switch (signal_default_action(sig)) {
            case SIGACT_TERM: {
                kprintf("[SIGNAL] pid %d terminated by signal %d\n", proc->pid, sig);
                process_exit(proc, 128 + sig);
                return;
            }
            case SIGACT_STOP: {
                if (!(proc->flags & PROC_FLAG_STOPPED)) {
                    proc->flags |= PROC_FLAG_STOPPED;
                    kprintf("[SIGNAL] pid %d stopped by signal %d\n", proc->pid, sig);
                    /* Block all threads in the process */
                    struct list_head* iter = proc->threads.next;
                    while (iter != &proc->threads) {
                        thread_t* t = container_of(iter, thread_t, threads_node);
                        iter = iter->next;
                        if (t->state == THREAD_READY || t->state == THREAD_RUNNING) {
                            sched_remove_thread(t);
                            t->state = THREAD_BLOCKED;
                        }
                    }
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
