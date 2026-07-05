#ifndef PERCPU_H
#define PERCPU_H

#include "types.h"
#include "smp.h"

/*
 * Per-CPU variable accessor macros.
 *
 * DEFINE_PER_CPU(type, name) creates a per-CPU variable in a special
 * .data..percpu section. Each CPU gets its own copy at boot via
 * smp_alloc_per_cpu().
 *
 * For now, we use the simpler approach: per-CPU data is accessed via
 * smp_this_cpu()->field or per_cpu(cpu)->field.
 * This avoids ELF section complexity while SMP is being implemented.
 */

#define this_cpu_ptr(field)  (&smp_this_cpu()->field)
#define per_cpu_ptr(cpu, field) (&smp_get_per_cpu(cpu)->field)

/* Shorthand for common per-CPU fields */
#define this_cpu_current()   ((void*)smp_this_cpu()->cpu_thread)
#define this_cpu_id()        (smp_this_cpu()->cpu_id)

#endif /* PERCPU_H */
