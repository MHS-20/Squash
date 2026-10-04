/* Checksums used by the container formats. All take a running value so a
 * stream can be checked in pieces; pass the documented initial value first. */
#ifndef SQ_CHECKSUM_H
#define SQ_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (gzip, zip, xz): reflected polynomial 0xEDB88320. Start with 0. */
uint32_t sq_crc32(uint32_t crc, const void *data, size_t n);
/* CRC-32C (Snappy framing): reflected Castagnoli polynomial. Start with 0. */
uint32_t sq_crc32c(uint32_t crc, const void *data, size_t n);
/* bzip2's CRC: same polynomial as CRC-32 but not reflected. Start with 0. */
uint32_t sq_crc32_bzip2(uint32_t crc, const void *data, size_t n);
/* Adler-32 (zlib). Start with 1. */
uint32_t sq_adler32(uint32_t adler, const void *data, size_t n);
/* xxHash (LZ4 frames use XXH32 with seed 0). One-shot only. */
uint32_t sq_xxh32(const void *data, size_t n, uint32_t seed);
uint64_t sq_xxh64(const void *data, size_t n, uint64_t seed);

#endif
