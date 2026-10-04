#include "huffman.h"

#include <stdlib.h>
#include <string.h>

#include "squash.h"

/* ---- building lengths ------------------------------------------------ */

typedef struct {
    uint64_t weight;
    int sym;
} leaf;

static int by_weight(const void *a, const void *b)
{
    const leaf *x = a, *y = b;
    if (x->weight != y->weight)
        return x->weight < y->weight ? -1 : 1;
    return x->sym - y->sym;  /* stable result across qsort implementations */
}

/* Unrestricted Huffman lengths by the two-queue method: leaves sorted by
 * weight form one queue, merged nodes form a second queue that is sorted by
 * construction, so the two lightest items are always at one of the heads. */
static int build_once(const uint64_t *w, int n, uint8_t *len)
{
    leaf leaves[HUFF_MAX_SYMS];
    uint64_t node_w[HUFF_MAX_SYMS];
    int parent[2 * HUFF_MAX_SYMS];  /* leaves 0..m-1, internal nodes m.. */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (w[i])
            leaves[m++] = (leaf){w[i], i};
    qsort(leaves, (size_t)m, sizeof *leaves, by_weight);

    int li = 0, ni = 0, nn = 0;  /* leaf head, node head, nodes made */
    for (int k = 0; k < m - 1; k++) {
        int pick[2];
        uint64_t sum = 0;
        for (int j = 0; j < 2; j++) {
            if (li < m && (ni == nn || leaves[li].weight <= node_w[ni])) {
                pick[j] = li;
                sum += leaves[li++].weight;
            } else {
                pick[j] = m + ni;
                sum += node_w[ni++];
            }
        }
        parent[pick[0]] = parent[pick[1]] = m + nn;
        node_w[nn++] = sum;
    }

    int longest = 0;
    int depth[2 * HUFF_MAX_SYMS];
    if (m > 0)
        depth[2 * m - 2] = 0;  /* the root is the last node made */
    /* Parents are created after children, so walking down from the root
     * resolves every depth in one reverse pass. */
    for (int i = 2 * m - 3; i >= 0; i--)
        depth[i] = depth[parent[i]] + 1;
    for (int i = 0; i < m; i++) {
        int d = m == 1 ? 1 : depth[i];
        len[leaves[i].sym] = (uint8_t)d;
        if (d > longest)
            longest = d;
    }
    return longest;
}

void huff_lengths(const uint32_t *freq, int n, int max_len, uint8_t *len)
{
    uint64_t w[HUFF_MAX_SYMS];
    memset(len, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        w[i] = freq[i];
    /* Length limiting by flattening, as bzip2 does: halve every weight
     * (keeping it non-zero) and rebuild until the tree is shallow enough.
     * Flatter weights give a more balanced tree. It is not optimal like
     * package-merge, but costs little ratio and is a few lines. */
    while (build_once(w, n, len) > max_len)
        for (int i = 0; i < n; i++)
            if (w[i])
                w[i] = 1 + w[i] / 2;
}

/* ---- canonical codes ------------------------------------------------- */

void huff_codes(const uint8_t *len, int n, uint32_t *code)
{
    uint32_t count[HUFF_MAX_BITS + 2] = {0}, next[HUFF_MAX_BITS + 2];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    /* First code of each length = (first code of previous length + its
     * count), shifted left: RFC 1951 section 3.2.2. */
    uint32_t c = 0;
    for (int b = 1; b <= HUFF_MAX_BITS; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? next[len[i]]++ : 0;
}

uint32_t huff_reverse(uint32_t code, int bits)
{
    uint32_t r = 0;
    for (int i = 0; i < bits; i++, code >>= 1)
        r = (r << 1) | (code & 1);
    return r;
}

/* Counts codes per length and rejects sets that would need more bit
 * patterns than exist ("over-subscribed"); such lengths describe no code. */
static int count_lengths(const uint8_t *len, int n, int max_bits, uint32_t *count)
{
    memset(count, 0, sizeof(uint32_t) * (HUFF_MAX_BITS + 1));
    for (int i = 0; i < n; i++) {
        if (len[i] > max_bits)
            return SQ_ERR_DATA;
        count[len[i]]++;
    }
    int64_t left = 1;  /* unused patterns at the current length */
    for (int b = 1; b <= max_bits; b++) {
        left = 2 * left - count[b];
        if (left < 0)
            return SQ_ERR_DATA;
    }
    return SQ_OK;
}

/* ---- LSB-first table ------------------------------------------------- */

int huff_table_build(huff_table *t, const uint8_t *len, int n, int max_bits)
{
    uint32_t count[HUFF_MAX_BITS + 1], code[HUFF_MAX_SYMS];
    if (n > HUFF_MAX_SYMS || max_bits > HUFF_TABLE_BITS)
        return SQ_ERR_ARG;
    int err = count_lengths(len, n, max_bits, count);
    if (err)
        return err;
    t->bits = max_bits;
    memset(t->entry, 0, sizeof(uint16_t) << max_bits);
    huff_codes(len, n, code);
    for (int s = 0; s < n; s++) {
        int l = len[s];
        if (!l)
            continue;
        /* A code of length l owns every peek whose low l bits match it;
         * the remaining high bits belong to whatever follows. */
        uint32_t r = huff_reverse(code[s], l);
        for (uint32_t i = r; i < (1u << max_bits); i += 1u << l)
            t->entry[i] = (uint16_t)(s << 4 | l);
    }
    return SQ_OK;
}

/* ---- MSB-first canonical --------------------------------------------- */

int huff_canon_build(huff_canon *h, const uint8_t *len, int n)
{
    uint32_t count[HUFF_MAX_BITS + 1];
    if (n > HUFF_MAX_SYMS)
        return SQ_ERR_ARG;
    int err = count_lengths(len, n, HUFF_MAX_BITS, count);
    if (err)
        return err;
    uint16_t offs[HUFF_MAX_BITS + 2];
    offs[1] = 0;
    h->max_len = 0;
    h->count[0] = 0;
    for (int b = 1; b <= HUFF_MAX_BITS; b++) {
        h->count[b] = (uint16_t)count[b];
        offs[b + 1] = (uint16_t)(offs[b] + count[b]);
        if (count[b])
            h->max_len = b;
    }
    for (int s = 0; s < n; s++)
        if (len[s])
            h->symbol[offs[len[s]]++] = (uint16_t)s;
    return SQ_OK;
}

int huff_decode_msb(const huff_canon *h, bitreader_msb *br)
{
    uint32_t bits = brm_peek(br, (unsigned)h->max_len);
    int32_t code = 0, first = 0, index = 0;
    /* Walk down one bit at a time: at each length, the codes of that length
     * are first..first+count-1, so a value in that range is a hit. */
    for (int l = 1; l <= h->max_len; l++) {
        code |= (bits >> (h->max_len - l)) & 1;
        int32_t count = h->count[l];
        if (code - first < count) {
            brm_consume(br, (unsigned)l);
            return h->symbol[index + code - first];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}
