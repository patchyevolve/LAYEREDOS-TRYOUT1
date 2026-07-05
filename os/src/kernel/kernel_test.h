#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H

#ifdef KERNEL_SELF_TEST
void kernel_self_test(void);
#else
#define kernel_self_test()
#endif

#endif
