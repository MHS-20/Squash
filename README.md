# Squash

A compression library (`libsquash`) and command-line tool (`squash`) in C11,
written from scratch from the format specifications.

## Build and test

```
make              # build/libsquash.a and build/squash
make test         # unit, round-trip, robustness and interop tests
make SAN=1 test   # the same under AddressSanitizer + UBSan (build-san/)
```

Interop tests use whichever official tools are installed and report the
rest as `SKIPPED`: `gzip`, Python's `zlib` module (zlib and raw DEFLATE),
`bzip2`, `lz4`, `xz`, `compress` (ncompress) and python-snappy. Point `PYTHON`
at an interpreter that has python-snappy, and put `compress` on `PATH`:

```
PATH=/path/to/ncompress:$PATH PYTHON=/path/to/venv/bin/python make test
```

## CLI

```
squash [-f FORMAT] [-d] [-l LEVEL] [-c] [-F] [IN [OUT]]
squash -L                       # list formats, suffixes and level ranges
```

- `-f` picks the format; when decompressing it is detected from the magic
  number if omitted (raw formats without a magic need `-f`).
- With no `IN`, reads standard input and writes standard output. With `IN`
  only, writes `IN` plus the format's suffix (or strips it with `-d`).
  `-c` forces standard output. The input file is never deleted, and an
  existing output is not overwritten without `-F`.

```
squash -f gzip -l 9 notes.txt          # -> notes.txt.gz
squash -d notes.txt.gz -c | less
tar c dir | squash -f gzip > dir.tar.gz
```

## Layout

| File | Concept |
|---|---|
| `src/squash.h` | public API and error codes |
| `src/buf.[ch]` | growable output buffer, shared LZ77 back-reference copy |
| `src/bitio.[ch]` | LSB-first and MSB-first bit readers and writers, backward reader (zstd) |
| `src/checksum.h` | checksum API |
| `src/crc32.c` | CRC-32, CRC-32C, bzip2 CRC (table driven) |
| `src/adler32.c` | Adler-32 |
| `src/xxhash.c` | xxHash32 / xxHash64 |
| `src/huffman.[ch]` | code lengths, length limit, canonical codes, LSB table and MSB canonical decoders |
| `src/lz77.[ch]` | LZ77 parsing: naive and hash-chain (greedy / lazy) match finders |
| `src/codec.[ch]` | format registry: names, suffixes, magic numbers, level ranges |
| `src/deflate.h` | DEFLATE constants and internal API |
| `src/deflate_tables.c` | length/distance base and extra-bit tables, fixed code |
| `src/deflate_dec.c` | DEFLATE decoder (inflate) |
| `src/deflate_enc.c` | DEFLATE encoder (LZ77 + per-block stored/fixed/dynamic choice) |
| `src/deflate.c` | raw DEFLATE front end |
| `src/zlib.c` | zlib container |
| `src/gzip.c` | gzip container (multi-member) |
| `src/cli.c` | the `squash` command-line tool |
| `tests/interop.sh` | both-direction interop with the official tools |
| `src/lzw.c` | LZW in the Unix `compress` (.Z) format |
| `src/lz4.h` | LZ4 constants and block API |
| `src/lz4_block.c` | LZ4 block format encoder and decoder |
| `src/lz4_frame.c` | LZ4 frame format (plus skippable and legacy frames) |
| `src/snappy.h` | Snappy constants and internal raw decoder |
| `src/snappy_raw.c` | Snappy raw format encoder and decoder |
| `src/snappy_frame.c` | Snappy framing format (chunks, masked CRC-32C) |
| `src/sais.[ch]` | suffix array by induced sorting (SA-IS) |
| `src/bwt.[ch]` | Burrows-Wheeler transform: naive, SA-IS based, inverse |
| `src/mtf.[ch]` | move-to-front transform |
| `src/rle.[ch]` | bzip2 RLE1 (byte runs) and RLE2 (RUNA/RUNB zero runs) |
| `src/bzip2.h` | bzip2 constants and internal API |
| `src/bzip2_rand.c` | randomised-block table and toggle |
| `src/bzip2_dec.c` | bzip2 decoder |
| `src/bzip2_enc.c` | bzip2 encoder (multi-table selection) |
| `src/arith.[ch]` | Witten-Neal-Cleary arithmetic coding, static and adaptive order-0 models |
| `src/rangecoder.[ch]` | LZMA-style binary range coder, bit trees, prices |
| `src/crc64.c` | CRC-64/XZ |
| `src/sha256.c` | SHA-256 (xz check) |
| `src/lzma.h` | LZMA model, decoder, encoder and LZMA2 API |
| `src/lzma_model.c` | LZMA probability model init/reset, properties byte |
| `src/lzma_dec.c` | LZMA decoder |
| `src/lzma_enc.c` | LZMA encoder (fast-mode parse) |
| `src/lzma_alone.c` | .lzma container |
| `src/lzma2.c` | LZMA2 chunking, both directions |
| `src/xz.c` | .xz container |
| `src/fse.[ch]` | FSE / tANS: normalisation, table description, coding tables, state machines, order-0 codec |
| `src/rans.[ch]` | rANS order-0 codec, single and two-way interleaved |
| `tests/test.h` | tiny test harness |
| `tests/test_*.c` | one unit test per module |
| `tests/corpus.h` | generated inputs: empty, 1 byte, zeros, random, text, binary, boundary sizes, >1 block |
| `tests/test_roundtrip.c` | every codec and level: compress, decompress, compare |
| `tests/test_robust.c` | truncated, bit-flipped and garbage input must fail cleanly |
