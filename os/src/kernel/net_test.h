#ifndef NET_TEST_H
#define NET_TEST_H

#ifdef NET_SELF_TEST
void net_self_test(void);
#else
#define net_self_test()
#endif

#endif
