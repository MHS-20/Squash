/* Generated test inputs covering the cases every codec must handle:
 * degenerate sizes, incompressible and maximally compressible data, text,
 * binary, and sizes straddling common window and block boundaries. */
#ifndef SQ_CORPUS_H
#define SQ_CORPUS_H

#include "test.h"

typedef struct {
    char name[48];
    unsigned char *data;
    size_t len;
} sample;

static inline void make_text(unsigned char *p, size_t n, unsigned seed)
{
    static const char *w[] = {"compression ", "the ", "of ", "and ", "huffman ",
                              "window ", "a ", "block ", "match ", "literal ",
                              "stream\n", "entropy, ", "is ", "Burrows-Wheeler "};
    unsigned char r[4096];
    fill_random(r, sizeof r, seed);
    size_t i = 0, k = 0;
    while (i < n) {
        const char *s = w[r[k++ % sizeof r] % 14];
        while (*s && i < n)
            p[i++] = (unsigned char)*s++;
    }
}

/* Structured binary: little-endian counters with noise, like a table dump. */
static inline void make_binary(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (i % 4 == 0) ? (unsigned char)(i / 4) : (i % 4 == 1) ? (unsigned char)(i >> 10)
                                                                    : (unsigned char)((i * 2654435761u) >> 29);
}

static inline size_t corpus_build(sample *s, size_t max, int large)
{
    size_t k = 0;
#define ADD(nm, len_)                                                       \
    do {                                                                    \
        if (k < max) {                                                      \
            snprintf(s[k].name, sizeof s[k].name, "%s", nm);                \
            s[k].len = (len_);                                              \
            s[k].data = malloc(s[k].len + 1);                               \
            k++;                                                            \
        }                                                                   \
    } while (0)
    ADD("empty", 0);
    ADD("one-byte", 1);
    s[k - 1].data[0] = 'x';
    ADD("zeros-100k", 100000);
    memset(s[k - 1].data, 0, 100000);
    ADD("random-70k", 70000);
    fill_random(s[k - 1].data, 70000, 11);
    ADD("text-200k", 200000);
    make_text(s[k - 1].data, 200000, 5);
    ADD("binary-150k", 150000);
    make_binary(s[k - 1].data, 150000);
    ADD("abab-5000", 5000);
    for (int i = 0; i < 5000; i++)
        s[k - 1].data[i] = "ab"[i % 2];
    static const size_t edges[] = {2, 3, 4, 5, 12, 13, 255, 256, 32767, 32768, 32769,
                                   65535, 65536, 65537, 65538, 262144};
    for (size_t e = 0; e < sizeof edges / sizeof edges[0]; e++) {
        char nm[48];
        snprintf(nm, sizeof nm, "text-%zu", edges[e]);
        ADD(nm, edges[e]);
        make_text(s[k - 1].data, edges[e], (unsigned)e);
    }
    if (large) {
        /* Larger than one bzip2 block (900k), one LZ4 block and frame. */
        ADD("text-2.5M", 2500000);
        make_text(s[k - 1].data, 2500000, 77);
        ADD("mixed-1.2M", 1200000);
        fill_random(s[k - 1].data, 400000, 3);
        make_binary(s[k - 1].data + 400000, 400000);
        memset(s[k - 1].data + 800000, 'q', 400000);
    }
#undef ADD
    return k;
}

static inline void corpus_free(sample *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(s[i].data);
}

#endif
