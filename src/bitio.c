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
