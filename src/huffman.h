/* Huffman coding: code lengths from frequencies, canonical code assignment,
 * and two decoders.
 *
 * Every format here transmits only code *lengths*; both sides derive the
 * actual bit patterns with the canonical rule (shorter codes first, ties by
 * symbol order), so lengths are the whole description of a code. */
#ifndef SQ_HUFFMAN_H
#define SQ_HUFFMAN_H

#include <stdint.h>

#include "bitio.h"

#define HUFF_MAX_SYMS 320
#define HUFF_MAX_BITS 20     /* bzip2 decoders must accept 20 */
#define HUFF_TABLE_BITS 15   /* DEFLATE's limit, sizes the LSB lookup table */

/* Fill len[0..n) with optimal-ish lengths for freq[], none above max_len.
 * Zero-frequency symbols get length 0. A lone used symbol gets length 1,
 * since a zero-bit code cannot be written. */
void huff_lengths(const uint32_t *freq, int n, int max_len, uint8_t *len);

/* Canonical code values (MSB-first, i.e. as read left to right). */
void huff_codes(const uint8_t *len, int n, uint32_t *code);

/* Reverse the low `bits` bits: DEFLATE sends codes starting from their most
 * significant bit, but packs bits LSB-first, so writers and the lookup table
 * both use the reversed pattern. */
uint32_t huff_reverse(uint32_t code, int bits);

/* ---- LSB-first lookup-table decoder (DEFLATE) ------------------------ */

/* One entry per possible `bits`-bit peek: (symbol << 4) | code length, or 0
 * for a bit pattern that no code starts with. One probe decodes a symbol. */
typedef struct {
    uint16_t entry[1 << HUFF_TABLE_BITS];
    int bits;
} huff_table;

/* SQ_ERR_DATA if the lengths are over-subscribed (more codes than bit
 * patterns). Incomplete sets are accepted; unused patterns decode as errors. */
int huff_table_build(huff_table *t, const uint8_t *len, int n, int max_bits);

/* Returns the symbol, or -1 for an invalid code. */
static inline int huff_decode_lsb(const huff_table *t, bitreader_lsb *br)
{
    uint16_t e = t->entry[brl_peek(br, (unsigned)t->bits)];
    if (e == 0)
        return -1;
    brl_consume(br, e & 15);
    return e >> 4;
}

/* ---- MSB-first canonical decoder (bzip2) ----------------------------- */

/* Codes of one length are consecutive integers, so per length we only need
 * how many codes there are and which symbols they map to, in order. This
 * stays small even for 20-bit codes, where a full table would not. */
typedef struct {
    uint16_t count[HUFF_MAX_BITS + 1];
    uint16_t symbol[HUFF_MAX_SYMS];
    int max_len;
} huff_canon;

int huff_canon_build(huff_canon *h, const uint8_t *len, int n);
int huff_decode_msb(const huff_canon *h, bitreader_msb *br);

#endif
