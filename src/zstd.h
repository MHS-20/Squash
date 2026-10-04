/* zstd internals shared by the decoder and encoder (RFC 8878). */
#ifndef SQ_ZSTD_H
#define SQ_ZSTD_H

#include "fse.h"
#include "huf0.h"
#include "squash.h"

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_DICT_MAGIC 0xEC30A437u
#define ZSTD_SKIPPABLE_MASK 0xFFFFFFF0u
#define ZSTD_SKIPPABLE 0x184D2A50u
#define ZSTD_BLOCK_MAX (128u * 1024)

enum { ZSTD_BLOCK_RAW = 0, ZSTD_BLOCK_RLE = 1, ZSTD_BLOCK_COMPRESSED = 2 };
enum { ZSTD_LIT_RAW = 0, ZSTD_LIT_RLE = 1, ZSTD_LIT_COMPRESSED = 2, ZSTD_LIT_TREELESS = 3 };
enum { ZSTD_MODE_PREDEFINED = 0, ZSTD_MODE_RLE = 1, ZSTD_MODE_FSE = 2, ZSTD_MODE_REPEAT = 3 };

/* The three sequence symbol types, in the order their tables appear. */
enum { ZSTD_LL = 0, ZSTD_OF = 1, ZSTD_ML = 2 };

#define ZSTD_LL_CODES 36
#define ZSTD_ML_CODES 53
#define ZSTD_OF_CODES 32   /* the decoder accepts offset codes up to 31 */
#define ZSTD_OF_MAX_CODE_DEFAULT 28

extern const uint32_t zstd_ll_base[ZSTD_LL_CODES];
extern const uint8_t zstd_ll_bits[ZSTD_LL_CODES];
extern const uint32_t zstd_ml_base[ZSTD_ML_CODES];
extern const uint8_t zstd_ml_bits[ZSTD_ML_CODES];

/* Predefined distributions and their accuracy logs; maximum logs for
 * tables sent in the stream. */
extern const fse_norm zstd_default_norm[3][ZSTD_ML_CODES];
extern const unsigned zstd_default_nsyms[3];
extern const unsigned zstd_default_log[3];
extern const unsigned zstd_max_log[3];

/* Repeat offsets: `offset_value` 1-3 refer to recent offsets (shifted by
 * one when the literal length is 0); larger values are offset + 3.
 * Resolves the actual offset and updates `rep` as the RFC prescribes;
 * returns 0 for the invalid "rep1 - 1 == 0" case. */
uint32_t zstd_resolve_offset(uint32_t rep[3], uint32_t offset_value, uint32_t lit_len);

#endif
