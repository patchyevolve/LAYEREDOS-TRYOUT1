#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#define TEST_PASS 0
#define TEST_FAIL 1

typedef int (*test_func_t)(void);

typedef struct {
    const char* name;
    test_func_t func;
} test_entry_t;

#define ASSERT_EQ(actual, expected, msg) do { \
    if ((actual) != (expected)) { \
        kprintf("[FAIL] %s: " msg " (got %llu, expected %llu)\n", \
                __func__, (unsigned long long)(uint64_t)(actual), \
                (unsigned long long)(uint64_t)(expected)); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_NE(actual, unexpected, msg) do { \
    if ((actual) == (unexpected)) { \
        kprintf("[FAIL] %s: " msg " (got %llu)\n", \
                __func__, (unsigned long long)(uint64_t)(actual)); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr, msg) do { \
    if (!(ptr)) { \
        kprintf("[FAIL] %s: " msg "\n", __func__); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_NULL(ptr, msg) do { \
    if ((ptr)) { \
        kprintf("[FAIL] %s: " msg "\n", __func__); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        kprintf("[FAIL] %s: " msg "\n", __func__); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_FALSE(cond, msg) do { \
    if ((cond)) { \
        kprintf("[FAIL] %s: " msg " (got true)\n", __func__); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_ERR_OK(err, msg) do { \
    if ((err) != ERR_OK) { \
        kprintf("[FAIL] %s: " msg " returned %d\n", __func__, (int)(err)); \
        return TEST_FAIL; \
    } \
} while(0)

#endif
