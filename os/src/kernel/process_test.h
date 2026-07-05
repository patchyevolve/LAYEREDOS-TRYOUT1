#ifndef PROCESS_TEST_H
#define PROCESS_TEST_H

#ifdef PROCESS_SELF_TEST
void process_self_test(void);
#else
#define process_self_test()
#endif

#endif
