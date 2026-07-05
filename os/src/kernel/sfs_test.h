#ifndef SFS_TEST_H
#define SFS_TEST_H

#ifdef SFS_SELF_TEST
void sfs_self_test(void);
#else
#define sfs_self_test()
#endif

#endif
