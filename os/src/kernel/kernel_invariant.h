#ifndef KERNEL_INVARIANT_H
#define KERNEL_INVARIANT_H

/* Runtime invariant check — panic on violation.
 * Zero-cost in release builds (gated by !NDEBUG).
 * Called once per schedule() invocation (~10-1000 Hz). */

#ifndef NDEBUG

void kernel_validate_invariants(void);

#else

#define kernel_validate_invariants()

#endif /* NDEBUG */

#endif /* KERNEL_INVARIANT_H */
