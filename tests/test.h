/* Minimal test harness: CHECK records a failure and keeps going, so one run
 * reports every broken case; TEST_DONE prints a summary and sets the exit. */
#ifndef SQ_TEST_H
#define SQ_TEST_H

#include <stdio.h>
#include <unistd.h>
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

/* Equal byte strings; memcmp's arguments may not be NULL even for n == 0. */
static inline int same_bytes(const void *a, size_t na, const void *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}

/* Runs `cmd` (a decompressor reading stdin) on `z` and compares its output
 * with `want`. Returns 1 on match, 0 on mismatch, -1 if `tool` is missing.
 * Lets unit tests check corner cases the CLI cannot produce. */
static inline int tool_decodes(const char *tool, const char *cmd, const void *z, size_t zn,
                               const void *want, size_t wn)
{
    char probe[128], line[256];
    snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1", tool);
    if (system(probe) != 0)
        return -1;
    char path[] = "/tmp/squash-test-XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f)
        return 0;
    fwrite(z, 1, zn, f);
    fclose(f);
    snprintf(line, sizeof line, "%s < %s", cmd, path);
    FILE *p = popen(line, "r");
    size_t cap = wn + 1, got = 0;
    unsigned char *buf = malloc(cap);
    while (p && buf && got < cap) {
        size_t k = fread(buf + got, 1, cap - got, p);
        if (k == 0)
            break;
        got += k;
    }
    int status = p ? pclose(p) : -1;
    remove(path);
    int ok = status == 0 && buf && same_bytes(buf, got, want, wn);
    free(buf);
    return ok;
}

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
