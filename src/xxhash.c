/* xxHash32 and xxHash64, one-shot. Inputs of 16 (32) bytes or more run four
 * independent accumulator lanes over stripes, which is where the speed comes
 * from; the tail and the final avalanche mix every bit into the result. */

#include "checksum.h"

#define P32_1 0x9E3779B1u
#define P32_2 0x85EBCA77u
#define P32_3 0xC2B2AE3Du
#define P32_4 0x27D4EB2Fu
#define P32_5 0x165667B1u

#define P64_1 0x9E3779B185EBCA87ull
#define P64_2 0xC2B2AE3D27D4EB4Full
#define P64_3 0x165667B19E3779F9ull
#define P64_4 0x85EBCA77C2B2AE63ull
#define P64_5 0x27D4EB2F165667C5ull

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

/* The format is defined little-endian regardless of host. */
static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static uint32_t round32(uint32_t acc, uint32_t lane)
{
    acc += lane * P32_2;
    return rotl32(acc, 13) * P32_1;
}

uint32_t sq_xxh32(const void *data, size_t n, uint32_t seed)
{
    const uint8_t *p = data, *end = p + n;
    uint32_t h;
    if (n >= 16) {
        uint32_t v1 = seed + P32_1 + P32_2, v2 = seed + P32_2;
        uint32_t v3 = seed, v4 = seed - P32_1;
        for (; end - p >= 16; p += 16) {
            v1 = round32(v1, le32(p));
            v2 = round32(v2, le32(p + 4));
            v3 = round32(v3, le32(p + 8));
            v4 = round32(v4, le32(p + 12));
        }
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P32_5;
    }
    h += (uint32_t)n;
    for (; end - p >= 4; p += 4)
        h = rotl32(h + le32(p) * P32_3, 17) * P32_4;
    for (; p < end; p++)
        h = rotl32(h + *p * P32_5, 11) * P32_1;
    h ^= h >> 15;
    h *= P32_2;
    h ^= h >> 13;
    h *= P32_3;
    h ^= h >> 16;
    return h;
}

static uint64_t round64(uint64_t acc, uint64_t lane)
{
    acc += lane * P64_2;
    return rotl64(acc, 31) * P64_1;
}

/* Folds one lane into the hash so no lane's entropy is lost. */
static uint64_t merge64(uint64_t h, uint64_t v)
{
    h ^= round64(0, v);
    return h * P64_1 + P64_4;
}

uint64_t sq_xxh64(const void *data, size_t n, uint64_t seed)
{
    const uint8_t *p = data, *end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + P64_1 + P64_2, v2 = seed + P64_2;
        uint64_t v3 = seed, v4 = seed - P64_1;
        for (; end - p >= 32; p += 32) {
            v1 = round64(v1, le64(p));
            v2 = round64(v2, le64(p + 8));
            v3 = round64(v3, le64(p + 16));
            v4 = round64(v4, le64(p + 24));
        }
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge64(h, v1);
        h = merge64(h, v2);
        h = merge64(h, v3);
        h = merge64(h, v4);
    } else {
        h = seed + P64_5;
    }
    h += (uint64_t)n;
    for (; end - p >= 8; p += 8) {
        h ^= round64(0, le64(p));
        h = rotl64(h, 27) * P64_1 + P64_4;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)le32(p) * P64_1;
        h = rotl64(h, 23) * P64_2 + P64_3;
        p += 4;
    }
    for (; p < end; p++) {
        h ^= *p * P64_5;
        h = rotl64(h, 11) * P64_1;
    }
    h ^= h >> 33;
    h *= P64_2;
    h ^= h >> 29;
    h *= P64_3;
    h ^= h >> 32;
    return h;
}
