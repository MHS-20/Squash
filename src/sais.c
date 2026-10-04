/* SA-IS.
 *
 * Each suffix is S-type if it is smaller than the next suffix, else L-type.
 * An LMS suffix is an S-type suffix whose predecessor is L-type. The key
 * fact: once the LMS suffixes are in sorted order, one left-to-right pass
 * places every L-type suffix and one right-to-left pass every S-type
 * suffix ("induced sorting"). The LMS suffixes themselves are sorted by
 * inducing once from an approximate order, naming the resulting LMS
 * substrings, and, if names repeat, recursing on the string of names,
 * which is at most half as long.
 *
 * The text gets a virtual sentinel: a unique smallest symbol appended at
 * the end, here symbol 0, with real symbols shifted up by one. */
#include "sais.h"

#include <stdlib.h>
#include <string.h>

#include "squash.h"

enum { L_TYPE = 0, S_TYPE = 1 };

static int is_lms(const uint8_t *t, int32_t i) { return i > 0 && t[i] == S_TYPE && t[i - 1] == L_TYPE; }

/* bkt[c] = start (or one past the end) of the bucket of symbol c. */
static void buckets(const int32_t *s, int32_t n, int32_t k, int32_t *bkt, int ends)
{
    memset(bkt, 0, sizeof(int32_t) * (size_t)k);
    for (int32_t i = 0; i < n; i++)
        bkt[s[i]]++;
    int32_t sum = 0;
    for (int32_t c = 0; c < k; c++) {
        sum += bkt[c];
        bkt[c] = ends ? sum : sum - bkt[c];
    }
}

/* From LMS suffixes already placed at their buckets' ends: L-types are
 * induced left to right from bucket starts, then S-types right to left
 * from bucket ends (overwriting the provisional LMS placement). */
static void induce(const int32_t *s, const uint8_t *t, int32_t *sa, int32_t n, int32_t k, int32_t *bkt)
{
    buckets(s, n, k, bkt, 0);
    for (int32_t i = 0; i < n; i++) {
        int32_t j = sa[i] - 1;
        if (sa[i] > 0 && t[j] == L_TYPE)
            sa[bkt[s[j]]++] = j;
    }
    buckets(s, n, k, bkt, 1);
    for (int32_t i = n - 1; i >= 0; i--) {
        int32_t j = sa[i] - 1;
        if (sa[i] > 0 && t[j] == S_TYPE)
            sa[--bkt[s[j]]] = j;
    }
}

/* Two LMS substrings (from an LMS position up to and including the next)
 * are equal if symbols and types match all the way. */
static int lms_equal(const int32_t *s, const uint8_t *t, int32_t n, int32_t a, int32_t b)
{
    for (int32_t d = 0;; d++) {
        if (a + d == n || b + d == n)
            return 0;  /* only the sentinel reaches the end, and it is unique */
        if (s[a + d] != s[b + d] || t[a + d] != t[b + d])
            return 0;
        if (d > 0 && (is_lms(t, a + d) || is_lms(t, b + d)))
            return is_lms(t, a + d) && is_lms(t, b + d);
    }
}

/* s[n-1] must be the unique smallest symbol (0); symbols are < k. */
static int sais_rec(const int32_t *s, int32_t *sa, int32_t n, int32_t k)
{
    uint8_t *t = malloc((size_t)n);
    int32_t *bkt = malloc(sizeof(int32_t) * (size_t)k);
    if (!t || !bkt) {
        free(t);
        free(bkt);
        return SQ_ERR_MEM;
    }
    t[n - 1] = S_TYPE;
    for (int32_t i = n - 2; i >= 0; i--)
        t[i] = s[i] < s[i + 1] || (s[i] == s[i + 1] && t[i + 1] == S_TYPE) ? S_TYPE : L_TYPE;

    /* Stage 1: sort LMS substrings by inducing from arbitrary LMS order. */
    buckets(s, n, k, bkt, 1);
    for (int32_t i = 0; i < n; i++)
        sa[i] = -1;
    for (int32_t i = 1; i < n; i++)
        if (is_lms(t, i))
            sa[--bkt[s[i]]] = i;
    induce(s, t, sa, n, k, bkt);

    /* Compact the now-sorted LMS positions to the front and name them:
     * equal substrings share a name, and names follow sorted order. */
    int32_t m = 0;
    for (int32_t i = 0; i < n; i++)
        if (is_lms(t, sa[i]))
            sa[m++] = sa[i];
    for (int32_t i = m; i < n; i++)
        sa[i] = -1;
    int32_t name = 0, prev = -1;
    for (int32_t i = 0; i < m; i++) {
        int32_t pos = sa[i];
        if (prev < 0 || !lms_equal(s, t, n, pos, prev))
            name++;
        prev = pos;
        /* LMS positions are at least 2 apart, so pos/2 slots never clash. */
        sa[m + pos / 2] = name - 1;
    }
    /* The reduced string: names in text order, at the end of sa. */
    int32_t j = n - 1;
    for (int32_t i = n - 1; i >= m; i--)
        if (sa[i] >= 0)
            sa[j--] = sa[i];
    int32_t *s1 = sa + n - m, *sa1 = sa;

    /* Stage 2: order the LMS suffixes exactly, recursing if names clash. */
    int err = SQ_OK;
    if (name < m) {
        err = sais_rec(s1, sa1, m, name);
    } else {
        for (int32_t i = 0; i < m; i++)
            sa1[s1[i]] = i;
    }
    if (err) {
        free(t);
        free(bkt);
        return err;
    }

    /* Map ranks back to text positions (reusing s1's slots), then place
     * the LMS suffixes in sorted order at their buckets' ends and induce
     * the final array. */
    for (int32_t i = 1, k1 = 0; i < n; i++)
        if (is_lms(t, i))
            s1[k1++] = i;
    for (int32_t i = 0; i < m; i++)
        sa1[i] = s1[sa1[i]];
    for (int32_t i = m; i < n; i++)
        sa[i] = -1;
    buckets(s, n, k, bkt, 1);
    for (int32_t i = m - 1; i >= 0; i--) {
        int32_t p = sa[i];
        sa[i] = -1;
        sa[--bkt[s[p]]] = p;
    }
    induce(s, t, sa, n, k, bkt);
    free(t);
    free(bkt);
    return SQ_OK;
}

int sais(const uint8_t *s, int32_t *sa, int32_t n)
{
    if (n <= 0)
        return n == 0 ? SQ_OK : SQ_ERR_ARG;
    int32_t *text = malloc(sizeof(int32_t) * ((size_t)n + 1));
    int32_t *full = malloc(sizeof(int32_t) * ((size_t)n + 1));
    if (!text || !full) {
        free(text);
        free(full);
        return SQ_ERR_MEM;
    }
    for (int32_t i = 0; i < n; i++)
        text[i] = s[i] + 1;
    text[n] = 0;
    int err = sais_rec(text, full, n + 1, 257);
    if (!err)
        memcpy(sa, full + 1, sizeof(int32_t) * (size_t)n);  /* drop the sentinel */
    free(text);
    free(full);
    return err;
}
