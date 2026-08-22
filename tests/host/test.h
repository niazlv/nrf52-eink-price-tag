#ifndef TEST_H
#define TEST_H

/* Minimal single-file test harness for the host-side lib/ tests.
 * One binary per module; run them all via `make test` in this directory. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_checks;
static int t_failures;

#define T_ASSERT_MSG(cond, ...) do {                                  \
    t_checks++;                                                        \
    if (!(cond)) {                                                     \
        t_failures++;                                                  \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
        printf(__VA_ARGS__);                                           \
        printf("\n");                                                  \
    }                                                                  \
} while (0)

#define T_ASSERT(cond) T_ASSERT_MSG(cond, "%s", #cond)

#define T_ASSERT_EQ(a, b) do {                                         \
    long long t_a = (long long)(a), t_b = (long long)(b);              \
    t_checks++;                                                        \
    if (t_a != t_b) {                                                  \
        t_failures++;                                                  \
        printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n",              \
               __FILE__, __LINE__, #a, #b, t_a, t_b);                  \
    }                                                                  \
} while (0)

#define T_RUN(fn) do { printf("- %s\n", #fn); fn(); } while (0)

static int t_report(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, t_checks, t_failures);
    return t_failures ? 1 : 0;
}

#endif /* TEST_H */
