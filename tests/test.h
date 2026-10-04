/* Minimal test harness: CHECK records a failure and keeps going, so one run
 * reports every broken case; TEST_DONE prints a summary and sets the exit. */
#ifndef SQ_TEST_H
#define SQ_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_checks, t_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        t_checks++;                                                          \
        if (!(cond)) {                                                       \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define CHECK_EQ_U(a, b)                                                     \
    do {                                                                     \
        unsigned long long a_ = (a), b_ = (b);                               \
        t_checks++;                                                          \
        if (a_ != b_) {                                                      \
            t_failures++;                                                    \
            fprintf(stderr, "%s:%d: %s == %s failed: 0x%llx != 0x%llx\n",    \
                    __FILE__, __LINE__, #a, #b, a_, b_);                     \
        }                                                                    \
    } while (0)

#define TEST_DONE(name)                                                      \
    do {                                                                     \
        printf("%-14s %s  (%d checks, %d failed)\n", name,                   \
               t_failures ? "FAIL" : "PASS", t_checks, t_failures);          \
        return t_failures ? 1 : 0;                                           \
    } while (0)

/* Deterministic pseudo-random bytes (xorshift) so failures reproduce. */
static inline void fill_random(unsigned char *p, size_t n, unsigned long long seed)
{
    unsigned long long x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        p[i] = (unsigned char)(x >> 32);
    }
}

#endif
