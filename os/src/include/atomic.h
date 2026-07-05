#ifndef ATOMIC_H
#define ATOMIC_H

#include "types.h"
#include "barrier.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { volatile int64_t counter; } atomic_t;
typedef struct { volatile uint64_t counter; } atomic64_t;

#define ATOMIC_INIT(i)  { (i) }

/* Atomic read (plain load, no barrier) */
static inline int64_t atomic_read(const atomic_t* v) {
    return v->counter;
}

static inline uint64_t atomic64_read(const atomic64_t* v) {
    return v->counter;
}

/* Atomic set (plain store, no barrier) */
static inline void atomic_set(atomic_t* v, int64_t i) {
    v->counter = i;
}

static inline void atomic64_set(atomic64_t* v, uint64_t i) {
    v->counter = i;
}

/* Atomic add */
static inline void atomic_add(int64_t i, atomic_t* v) {
    asm volatile("lock addq %1, %0" : "+m"(v->counter) : "r"(i) : "cc");
}

static inline void atomic64_add(uint64_t i, atomic64_t* v) {
    asm volatile("lock addq %1, %0" : "+m"(v->counter) : "r"(i) : "cc");
}

/* Atomic sub */
static inline void atomic_sub(int64_t i, atomic_t* v) {
    asm volatile("lock subq %1, %0" : "+m"(v->counter) : "r"(i) : "cc");
}

static inline void atomic64_sub(uint64_t i, atomic64_t* v) {
    asm volatile("lock subq %1, %0" : "+m"(v->counter) : "r"(i) : "cc");
}

/* Atomic increment / decrement */
static inline void atomic_inc(atomic_t* v) {
    asm volatile("lock incq %0" : "+m"(v->counter) : : "cc");
}

static inline void atomic_dec(atomic_t* v) {
    asm volatile("lock decq %0" : "+m"(v->counter) : : "cc");
}

static inline void atomic64_inc(atomic64_t* v) {
    asm volatile("lock incq %0" : "+m"(v->counter) : : "cc");
}

static inline void atomic64_dec(atomic64_t* v) {
    asm volatile("lock decq %0" : "+m"(v->counter) : : "cc");
}

/* Add and return result */
static inline int64_t atomic_add_return(int64_t i, atomic_t* v) {
    int64_t tmp = i;
    asm volatile("lock xaddq %0, %1" : "+r"(tmp), "+m"(v->counter) : : "cc");
    return tmp + i;
}

static inline uint64_t atomic64_add_return(uint64_t i, atomic64_t* v) {
    uint64_t tmp = i;
    asm volatile("lock xaddq %0, %1" : "+r"(tmp), "+m"(v->counter) : : "cc");
    return tmp + i;
}

/* Sub and test zero */
static inline int atomic_sub_and_test(int64_t i, atomic_t* v) {
    unsigned char zf;
    asm volatile("lock subq %2, %0; setz %1"
                 : "+m"(v->counter), "=q"(zf) : "r"(i) : "cc");
    return zf;
}

/* Compare and exchange */
static inline int64_t atomic_cmpxchg(atomic_t* v, int64_t old, int64_t new) {
    int64_t ret;
    asm volatile("lock cmpxchgq %2, %1"
                 : "=a"(ret), "+m"(v->counter) : "r"(new), "a"(old) : "cc");
    return ret;
}

static inline uint64_t atomic64_cmpxchg(atomic64_t* v, uint64_t old, uint64_t new) {
    uint64_t ret;
    asm volatile("lock cmpxchgq %2, %1"
                 : "=a"(ret), "+m"(v->counter) : "r"(new), "a"(old) : "cc");
    return ret;
}

/* Exchange */
static inline int64_t atomic_xchg(atomic_t* v, int64_t new) {
    int64_t ret;
    asm volatile("lock xchgq %0, %1"
                 : "=r"(ret), "+m"(v->counter) : "0"(new) : "cc");
    return ret;
}

static inline uint64_t atomic64_xchg(atomic64_t* v, uint64_t new) {
    uint64_t ret;
    asm volatile("lock xchgq %0, %1"
                 : "=r"(ret), "+m"(v->counter) : "0"(new) : "cc");
    return ret;
}

/* Atomic bit operations on unsigned long (8 bytes) */
static inline void atomic_set_bit(int nr, volatile void* addr) {
    asm volatile("lock btsq %1, %0"
                 : "+m"(*(volatile unsigned long*)addr)
                 : "r"((long)nr) : "cc");
}

static inline void atomic_clear_bit(int nr, volatile void* addr) {
    asm volatile("lock btrq %1, %0"
                 : "+m"(*(volatile unsigned long*)addr)
                 : "r"((long)nr) : "cc");
}

static inline int atomic_test_bit(int nr, const volatile void* addr) {
    unsigned char bit;
    asm volatile("btq %2, %1; setc %0"
                 : "=q"(bit) : "m"(*(const volatile unsigned long*)addr),
                   "r"((long)nr) : "cc");
    return bit;
}

static inline int atomic_test_and_set_bit(int nr, volatile void* addr) {
    unsigned char bit;
    asm volatile("lock btsq %2, %0; setc %1"
                 : "+m"(*(volatile unsigned long*)addr), "=q"(bit)
                 : "r"((long)nr) : "cc");
    return bit;
}

static inline int atomic_test_and_clear_bit(int nr, volatile void* addr) {
    unsigned char bit;
    asm volatile("lock btrq %2, %0; setc %1"
                 : "+m"(*(volatile unsigned long*)addr), "=q"(bit)
                 : "r"((long)nr) : "cc");
    return bit;
}

#ifdef __cplusplus
}
#endif

#endif /* ATOMIC_H */
