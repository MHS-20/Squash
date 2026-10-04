/* DEFLATE decoder (RFC 1951): stored, fixed-Huffman and dynamic-Huffman
 * blocks, decoded into one growing buffer that doubles as the window. */
#include <stdlib.h>

#include "bitio.h"
#include "deflate.h"
#include "huffman.h"

typedef struct {
    bitreader_lsb br;
    sq_buf *out;
    size_t floor;  /* back-references may not reach before this stream */
    huff_table litlen, dist, codelen;
} inflater;

static int stored_block(inflater *z)
{
    brl_align(&z->br);
    uint32_t len = brl_read(&z->br, 16);
    uint32_t nlen = brl_read(&z->br, 16);
    if (z->br.overrun || len != (~nlen & 0xFFFF))
        return SQ_ERR_DATA;
    int err = sq_buf_reserve(z->out, len);
    for (uint32_t i = 0; !err && i < len; i++)
        err = sq_buf_push(z->out, (uint8_t)brl_read(&z->br, 8));
    return err ? err : z->br.overrun ? SQ_ERR_DATA : SQ_OK;
}

/* Reads the code-length code, then the literal/length and distance code
 * lengths it encodes (RFC 1951 section 3.2.7), and builds both tables. */
static int dynamic_tables(inflater *z)
{
    bitreader_lsb *br = &z->br;
    int nlit = (int)brl_read(br, 5) + 257;
    int ndist = (int)brl_read(br, 5) + 1;
    int nclen = (int)brl_read(br, 4) + 4;
    if (nlit > DEFLATE_NUM_LITLEN || ndist > DEFLATE_NUM_DIST)
        return SQ_ERR_DATA;

    uint8_t clen[DEFLATE_NUM_CODELEN] = {0};
    for (int i = 0; i < nclen; i++)
        clen[deflate_codelen_order[i]] = (uint8_t)brl_read(br, 3);
    if (huff_table_build(&z->codelen, clen, DEFLATE_NUM_CODELEN, 7))
        return SQ_ERR_DATA;

    /* Literal/length and distance lengths form one sequence, so a repeat
     * may run from one into the other. */
    uint8_t lens[DEFLATE_NUM_LITLEN + DEFLATE_NUM_DIST];
    int have = 0;
    while (have < nlit + ndist) {
        int sym = huff_decode_lsb(&z->codelen, br);
        if (sym < 0 || br->overrun)
            return SQ_ERR_DATA;
        if (sym < 16) {
            lens[have++] = (uint8_t)sym;
            continue;
        }
        uint8_t val = 0;
        int rep;
        if (sym == 16) {
            if (have == 0)
                return SQ_ERR_DATA;  /* nothing to repeat */
            val = lens[have - 1];
            rep = 3 + (int)brl_read(br, 2);
        } else if (sym == 17) {
            rep = 3 + (int)brl_read(br, 3);
        } else {
            rep = 11 + (int)brl_read(br, 7);
        }
        if (have + rep > nlit + ndist)
            return SQ_ERR_DATA;
        while (rep--)
            lens[have++] = val;
    }
    if (lens[DEFLATE_END_BLOCK] == 0)
        return SQ_ERR_DATA;  /* a block that could never end */
    if (huff_table_build(&z->litlen, lens, nlit, 15) ||
        huff_table_build(&z->dist, lens + nlit, ndist, 15))
        return SQ_ERR_DATA;
    return br->overrun ? SQ_ERR_DATA : SQ_OK;
}

static int fixed_tables(inflater *z)
{
    uint8_t litlen[288], dist[30];
    deflate_fixed_lengths(litlen, dist);
    huff_table_build(&z->litlen, litlen, 288, 15);
    huff_table_build(&z->dist, dist, 30, 15);
    return SQ_OK;
}

/* Literal/length symbols until end-of-block. */
static int huffman_block(inflater *z)
{
    bitreader_lsb *br = &z->br;
    for (;;) {
        int sym = huff_decode_lsb(&z->litlen, br);
        /* Checking overrun here also bounds the loop: past the end, every
         * decode consumes bits that do not exist. */
        if (sym < 0 || br->overrun)
            return SQ_ERR_DATA;
        if (sym < 256) {
            int err = sq_buf_push(z->out, (uint8_t)sym);
            if (err)
                return err;
            continue;
        }
        if (sym == DEFLATE_END_BLOCK)
            return SQ_OK;
        sym -= 257;
        if (sym >= 29)
            return SQ_ERR_DATA;  /* codes 286 and 287 are reserved */
        uint32_t len = deflate_len_base[sym] + brl_read(br, deflate_len_extra[sym]);
        int dsym = huff_decode_lsb(&z->dist, br);
        if (dsym < 0 || dsym >= DEFLATE_NUM_DIST)
            return SQ_ERR_DATA;
        uint32_t dist = deflate_dist_base[dsym] + brl_read(br, deflate_dist_extra[dsym]);
        if (br->overrun)
            return SQ_ERR_DATA;
        int err = sq_buf_lzcopy(z->out, z->floor, dist, len);
        if (err)
            return err;
    }
}

int sq_inflate(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    inflater *z = malloc(sizeof *z);  /* the tables are ~130 KB */
    if (!z)
        return SQ_ERR_MEM;
    brl_init(&z->br, in, n);
    z->out = out;
    z->floor = out->len;
    int final = 0, err = SQ_OK;
    while (!err && !final) {
        final = (int)brl_read(&z->br, 1);
        switch (brl_read(&z->br, 2)) {
        case 0:
            err = stored_block(z);
            break;
        case 1:
            err = fixed_tables(z);
            if (!err)
                err = huffman_block(z);
            break;
        case 2:
            err = dynamic_tables(z);
            if (!err)
                err = huffman_block(z);
            break;
        default:
            err = SQ_ERR_DATA;  /* block type 3 is reserved */
        }
        if (!err && z->br.overrun)
            err = SQ_ERR_DATA;
    }
    if (!err) {
        /* The stream ends mid-byte; containers resume at the next byte. */
        brl_align(&z->br);
        *used = brl_tell(&z->br, in);
    }
    free(z);
    return err;
}
