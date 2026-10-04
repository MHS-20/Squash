#include "bitio.h"

#include "squash.h"

void brl_init(bitreader_lsb *br, const uint8_t *p, size_t n)
{
    br->p = p;
    br->end = p + n;
    br->bits = 0;
    br->count = 0;
    br->overrun = 0;
}

void brm_init(bitreader_msb *br, const uint8_t *p, size_t n)
{
    br->p = p;
    br->end = p + n;
    br->bits = 0;
    br->count = 0;
    br->overrun = 0;
}

void bwl_init(bitwriter_lsb *bw, sq_buf *out)
{
    bw->out = out;
    bw->bits = 0;
    bw->count = 0;
    bw->err = SQ_OK;
}

void bwm_init(bitwriter_msb *bw, sq_buf *out)
{
    bw->out = out;
    bw->bits = 0;
    bw->count = 0;
    bw->err = SQ_OK;
}

int bwl_flush(bitwriter_lsb *bw)
{
    if (bw->count > 0)
        bwl_put(bw, 0, 8 - bw->count);
    return bw->err;
}

int bwm_flush(bitwriter_msb *bw)
{
    if (bw->count > 0)
        bwm_put(bw, 0, 8 - bw->count);
    return bw->err;
}

int brr_init(bitreader_rev *br, const uint8_t *p, size_t n)
{
    if (n == 0 || p[n - 1] == 0)
        return SQ_ERR_DATA;
    int top = 7;
    while (!(p[n - 1] & (1u << top)))
        top--;
    br->start = p;
    br->bits = 8 * (n - 1) + (size_t)top;  /* everything below the marker */
    br->overrun = 0;
    return SQ_OK;
}

int bwl_close_rev(bitwriter_lsb *bw)
{
    bwl_put(bw, 1, 1);
    return bwl_flush(bw);
}
