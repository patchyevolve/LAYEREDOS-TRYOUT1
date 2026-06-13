#ifndef STORAGE_TEST_H
#define STORAGE_TEST_H

#ifdef STORAGE_SELF_TEST
void storage_self_test(void);
#else
#define storage_self_test()
#endif

#endif
