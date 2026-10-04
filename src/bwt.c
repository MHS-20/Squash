#include "bwt.h"

#include <stdlib.h>
#include <string.h>

#include "sais.h"
#include "squash.h"

/* qsort has no context argument, so the comparator reads the text from
 * here; the naive transform is test-only and single-threaded. */
static const uint8_t *rot_text;
static int32_t rot_n;

static int cmp_rotations(const void *a, const void *b)
{
    int32_t i = *(const int32_t *)a, j = *(const int32_t *)b;
    for (int32_t k = 0; k < rot_n; k++) {
        uint8_t x = rot_text[(i + k) % rot_n], y = rot_text[(j + k) % rot_n];
        if (x != y)
            return x - y;
    }
    return 0;  /* equal rotations: either order gives the same output */
}

int bwt_naive(const uint8_t *in, int32_t n, uint8_t *out, int32_t *primary)
{
    int32_t *rot = malloc(sizeof(int32_t) * (size_t)(n ? n : 1));
    if (!rot)
        return SQ_ERR_MEM;
    for (int32_t i = 0; i < n; i++)
        rot[i] = i;
    rot_text = in;
    rot_n = n;
    qsort(rot, (size_t)n, sizeof *rot, cmp_rotations);
    *primary = 0;
    for (int32_t i = 0; i < n; i++) {
        out[i] = in[(rot[i] + n - 1) % n];  /* last column = preceding byte */
        if (rot[i] == 0)
            *primary = i;
    }
    free(rot);
    return SQ_OK;
}

int bwt_forward(const uint8_t *in, int32_t n, uint8_t *out, int32_t *primary)
{
    /* Rotations of S are the length-n prefixes of the suffixes of SS that
     * start in the first half, so sorting those suffixes sorts the
     * rotations (ties are equal rotations, whose order does not matter). */
    *primary = 0;
    if (n == 0)
        return SQ_OK;
    if (n > INT32_MAX / 2)
        return SQ_ERR_ARG;
    uint8_t *ss = malloc(2 * (size_t)n);
    int32_t *sa = malloc(sizeof(int32_t) * 2 * (size_t)n);
    int err = ss && sa ? SQ_OK : SQ_ERR_MEM;
    if (!err) {
        memcpy(ss, in, (size_t)n);
        memcpy(ss + n, in, (size_t)n);
        err = sais(ss, sa, 2 * n);
    }
    for (int32_t i = 0, row = 0; !err && i < 2 * n; i++) {
        int32_t p = sa[i];
        if (p >= n)
            continue;
        if (p == 0)
            *primary = row;
        out[row++] = in[(p + n - 1) % n];
    }
    free(ss);
    free(sa);
    return err;
}

int bwt_inverse(const uint8_t *in, int32_t n, int32_t primary, uint8_t *out)
{
    if (n == 0)
        return SQ_OK;
    if (primary < 0 || primary >= n)
        return SQ_ERR_DATA;
    uint32_t *next = malloc(sizeof(uint32_t) * (size_t)n);
    if (!next)
        return SQ_ERR_MEM;
    /* The k-th occurrence of byte c in the first column and in the last
     * column are the same text position. So walking from a row's first
     * byte to the row whose last byte is that occurrence steps forward
     * one position in the text. */
    int32_t start[256] = {0};
    for (int32_t i = 0; i < n; i++)
        start[in[i]]++;
    for (int32_t c = 0, sum = 0; c < 256; c++) {
        int32_t cnt = start[c];
        start[c] = sum;
        sum += cnt;
    }
    for (int32_t i = 0; i < n; i++)
        next[start[in[i]]++] = (uint32_t)i;
    uint32_t p = (uint32_t)primary;
    for (int32_t k = 0; k < n; k++) {
        p = next[p];
        out[k] = in[p];
    }
    free(next);
    return SQ_OK;
}
