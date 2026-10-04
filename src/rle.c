#include "rle.h"

int rle1_encode(const uint8_t *in, size_t n, size_t *used, sq_buf *out, size_t cap)
{
    size_t i = 0, start = out->len;
    while (i < n) {
        uint8_t c = in[i];
        size_t run = 1;
        while (i + run < n && run < 255 && in[i + run] == c)
            run++;
        size_t cost = run < 4 ? run : 5;
        if (out->len - start + cost > cap)
            break;
        int err = SQ_OK;
        for (size_t k = 0; k < (run < 4 ? run : 4) && !err; k++)
            err = sq_buf_push(out, c);
        if (!err && run >= 4)
            err = sq_buf_push(out, (uint8_t)(run - 4));
        if (err)
            return err;
        i += run;
    }
    *used = i;
    return SQ_OK;
}

int rle1_decode(const uint8_t *in, size_t n, sq_buf *out)
{
    int same = 0;  /* how many times the previous byte has repeated */
    int prev = -1;
    for (size_t i = 0; i < n; i++) {
        int err;
        if (same == 4) {
            /* After four equal bytes, the next byte is a repeat count. */
            int err2 = sq_buf_reserve(out, in[i]);
            if (err2)
                return err2;
            for (int k = 0; k < in[i]; k++)
                out->data[out->len++] = (uint8_t)prev;
            same = 0;
            prev = -1;
            continue;
        }
        same = in[i] == prev ? same + 1 : 1;
        prev = in[i];
        err = sq_buf_push(out, in[i]);
        if (err)
            return err;
    }
    return SQ_OK;
}

/* Emits a zero run of length r: bijective base 2, digits 1 (RUNA) and 2
 * (RUNB), least significant first. */
static size_t put_run(size_t r, uint16_t *out)
{
    size_t k = 0;
    for (r--;; r = (r - 2) / 2) {
        out[k++] = (r & 1) ? RLE2_RUNB : RLE2_RUNA;
        if (r < 2)
            break;
    }
    return k;
}

size_t rle2_encode(const uint8_t *in, size_t n, uint16_t *out)
{
    size_t k = 0, zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            zeros++;
            continue;
        }
        if (zeros) {
            k += put_run(zeros, out + k);
            zeros = 0;
        }
        out[k++] = (uint16_t)(in[i] + 1);
    }
    if (zeros)
        k += put_run(zeros, out + k);
    return k;
}

int64_t rle2_decode(const uint16_t *in, size_t n, uint8_t *out, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        if (in[i] > RLE2_RUNB) {
            if (in[i] > 256 || k == max)
                return SQ_ERR_DATA;
            out[k++] = (uint8_t)(in[i++] - 1);
            continue;
        }
        size_t run = 0, weight = 1;
        for (; i < n && in[i] <= RLE2_RUNB; i++) {
            /* Bounding the digit weight keeps `run` from overflowing; real
             * runs never exceed a block (900k), as bzip2 also checks. */
            if (weight > max)
                return SQ_ERR_DATA;
            run += weight * (in[i] == RLE2_RUNA ? 1 : 2);
            weight *= 2;
        }
        if (run > max - k)
            return SQ_ERR_DATA;
        for (size_t j = 0; j < run; j++)
            out[k++] = 0;
    }
    return (int64_t)k;
}
