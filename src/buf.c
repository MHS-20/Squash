#include "buf.h"

#include <stdlib.h>
#include <string.h>

#include "squash.h"

void sq_buf_init(sq_buf *b) { b->data = NULL; b->len = b->cap = 0; }

void sq_buf_free(sq_buf *b) { free(b->data); sq_buf_init(b); }

int sq_buf_reserve(sq_buf *b, size_t extra)
{
    if (extra > SQ_BUF_MAX - b->len)
        return SQ_ERR_MEM;
    size_t need = b->len + extra;
    if (need <= b->cap)
        return SQ_OK;
    /* Doubling keeps appends amortised O(1). */
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return SQ_ERR_MEM;
    b->data = p;
    b->cap = cap;
    return SQ_OK;
}

int sq_buf_push(sq_buf *b, uint8_t byte)
{
    if (b->len == b->cap) {
        int err = sq_buf_reserve(b, 1);
        if (err)
            return err;
    }
    b->data[b->len++] = byte;
    return SQ_OK;
}

int sq_buf_append(sq_buf *b, const void *src, size_t n)
{
    if (n == 0)
        return SQ_OK;
    int err = sq_buf_reserve(b, n);
    if (err)
        return err;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return SQ_OK;
}

int sq_buf_put_le(sq_buf *b, uint64_t v, int nbytes)
{
    for (int i = 0; i < nbytes; i++) {
        int err = sq_buf_push(b, (uint8_t)(v >> (8 * i)));
        if (err)
            return err;
    }
    return SQ_OK;
}

int sq_buf_put_be(sq_buf *b, uint64_t v, int nbytes)
{
    for (int i = nbytes - 1; i >= 0; i--) {
        int err = sq_buf_push(b, (uint8_t)(v >> (8 * i)));
        if (err)
            return err;
    }
    return SQ_OK;
}

int sq_buf_lzcopy(sq_buf *b, size_t floor, size_t dist, size_t len)
{
    if (dist == 0 || dist > b->len - floor)
        return SQ_ERR_DATA;
    int err = sq_buf_reserve(b, len);
    if (err)
        return err;
    uint8_t *dst = b->data + b->len;
    const uint8_t *src = dst - dist;
    /* Byte at a time on purpose: when dist < len the source overlaps bytes
     * this very loop is writing, which is how runs are encoded. */
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
    b->len += len;
    return SQ_OK;
}
