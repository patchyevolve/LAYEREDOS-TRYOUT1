#ifndef SECURITY_TEST_H
#define SECURITY_TEST_H

#ifdef SECURITY_SELF_TEST
void security_self_test(void);
#else
#define security_self_test()
#endif

#endif
