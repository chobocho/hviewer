/*
 * minitest.h — 의존성 없는 단위 테스트 매크로
 *
 * 사용:
 *   TEST(name) { ASSERT_EQ(2 + 2, 4); }
 *   int main(void) { RUN(name); TEST_SUMMARY(); }
 *
 * 외부 라이브러리를 쓰지 않는 hview 정책에 맞춰 stdio + 매크로만 사용.
 */

#ifndef MINITEST_H
#define MINITEST_H

#include <stdio.h>
#include <string.h>

static int  g_test_passed = 0;
static int  g_test_failed = 0;
static int  g_test_local_failed = 0;
static const char *g_current_test = "";

#define TEST(name)        static void test_##name(void)

#define RUN(name)         do {                                          \
    g_current_test = #name;                                             \
    g_test_local_failed = 0;                                            \
    test_##name();                                                      \
    if (g_test_local_failed == 0) {                                     \
        printf("  PASS  %s\n", #name);                                  \
        g_test_passed++;                                                \
    } else {                                                            \
        printf("  FAIL  %s (%d assertion(s))\n",                        \
               #name, g_test_local_failed);                             \
        g_test_failed++;                                                \
    }                                                                   \
} while (0)

#define MT_FAIL_(fmt, ...) do {                                         \
    printf("    [%s:%d in %s] " fmt "\n",                               \
           __FILE__, __LINE__, g_current_test, __VA_ARGS__);            \
    g_test_local_failed++;                                              \
} while (0)

#define ASSERT_TRUE(expr) do {                                          \
    if (!(expr)) MT_FAIL_("ASSERT_TRUE(%s)", #expr);                    \
} while (0)

#define ASSERT_FALSE(expr) do {                                         \
    if ((expr)) MT_FAIL_("ASSERT_FALSE(%s)", #expr);                    \
} while (0)

#define ASSERT_EQ(a, b) do {                                            \
    long long _a = (long long)(a);                                      \
    long long _b = (long long)(b);                                      \
    if (_a != _b)                                                       \
        MT_FAIL_("ASSERT_EQ(%s=%lld, %s=%lld)", #a, _a, #b, _b);        \
} while (0)

#define ASSERT_NE(a, b) do {                                            \
    long long _a = (long long)(a);                                      \
    long long _b = (long long)(b);                                      \
    if (_a == _b)                                                       \
        MT_FAIL_("ASSERT_NE(%s=%lld, %s)", #a, _a, #b);                 \
} while (0)

#define ASSERT_GE(a, b) do {                                            \
    long long _a = (long long)(a);                                      \
    long long _b = (long long)(b);                                      \
    if (_a < _b)                                                        \
        MT_FAIL_("ASSERT_GE(%s=%lld, %s=%lld)", #a, _a, #b, _b);        \
} while (0)

#define TEST_SUMMARY() do {                                             \
    printf("\n%d test(s) passed, %d failed\n",                          \
           g_test_passed, g_test_failed);                               \
    return g_test_failed > 0 ? 1 : 0;                                   \
} while (0)

#endif /* MINITEST_H */
