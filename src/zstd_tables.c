#include "zstd.h"

const uint32_t zstd_ll_base[ZSTD_LL_CODES] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40,
    48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t zstd_ll_bits[ZSTD_LL_CODES] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

const uint32_t zstd_ml_base[ZSTD_ML_CODES] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41,
    43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t zstd_ml_bits[ZSTD_ML_CODES] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

const fse_norm zstd_default_norm[3][ZSTD_ML_CODES] = {
    [ZSTD_LL] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1},
    [ZSTD_OF] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
                 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1},
    [ZSTD_ML] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1},
};
const unsigned zstd_default_nsyms[3] = {36, 29, 53};
const unsigned zstd_default_log[3] = {6, 5, 6};
const unsigned zstd_max_log[3] = {9, 8, 9};

uint32_t zstd_resolve_offset(uint32_t rep[3], uint32_t offset_value, uint32_t lit_len)
{
    if (offset_value > 3) {
        rep[2] = rep[1];
        rep[1] = rep[0];
        rep[0] = offset_value - 3;
        return rep[0];
    }
    /* With no literals, "repeat the last offset" would be pointless (the
     * previous match would simply have been longer), so the codes shift:
     * 1 -> rep2, 2 -> rep3, 3 -> rep1 - 1. */
    uint32_t idx = offset_value - 1 + (lit_len == 0);
    uint32_t offset;
    if (idx == 3) {
        offset = rep[0] - 1;
        if (offset == 0)
            return 0;
    } else {
        offset = rep[idx];
    }
    /* Move the used offset to the front. */
    if (idx >= 1) {
        if (idx >= 2)
            rep[2] = rep[1];
        rep[1] = rep[0];
        rep[0] = offset;
    }
    return offset;
}
