#ifndef BARRIER_H
#define BARRIER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Full memory barrier */
static inline void mb(void) {
    asm volatile("mfence" ::: "memory");
}

/* Read barrier */
static inline void rmb(void) {
    asm volatile("lfence" ::: "memory");
}

/* Write barrier */
static inline void wmb(void) {
    asm volatile("sfence" ::: "memory");
}

/* SMP-specific barriers — compile to real barriers on SMP, empty on UP */
#ifdef CONFIG_SMP
#define smp_mb()    mb()
#define smp_rmb()   rmb()
#define smp_wmb()   wmb()
#else
#define smp_mb()    asm volatile("" ::: "memory")
#define smp_rmb()   asm volatile("" ::: "memory")
#define smp_wmb()   asm volatile("" ::: "memory")
#endif

/* CPU relaxation hint for spin-wait loops */
static inline void cpu_relax(void) {
    asm volatile("rep; nop" ::: "memory");
}

#ifdef __cplusplus
}
#endif

#endif /* BARRIER_H */
