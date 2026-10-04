# Squash

A compression library (`libsquash`) and command-line tool (`squash`) in C11,
written from scratch from the format specifications.

## Formats

| Format | `-f` | Levels | Encoder | Decoder also handles |
|---|---|---|---|---|
| gzip (RFC 1952) | `gzip` | 1-9 (6) | LZ77 hash chains, greedy 1-3 / lazy 4-9; stored, fixed or dynamic Huffman per block | all header flags, multiple members |
| zlib (RFC 1950) | `zlib` | 1-9 (6) | as gzip | preset dictionary reported as unsupported |
| raw DEFLATE (RFC 1951) | `deflate` | 1-9 (6) | as gzip | |
| Unix compress (.Z) | `lzw` | 9-16 bits (16) | LZW with compress's ratio-based reset | non-block-mode streams |
| LZ4 frame | `lz4` | 1-9 (1) | hash chains, 4 MB independent blocks, content checksum | linked blocks, block checksums, skippable and legacy frames |
| Snappy framing | `snappy` | 1 | 64 KB chunks, masked CRC-32C | padding and skippable chunks |
| Snappy raw | `snappy-raw` | 1 | | |
| bzip2 | `bzip2` | 1-9 (9) | RLE1, BWT via SA-IS, MTF, RLE2, 2-6 Huffman tables | randomised blocks, multiple streams |
| xz | `xz` | 1-9 (6) | LZMA2; fast parser 1-6, optimal parser 7-9; CRC64 | multiple blocks and streams, CRC32/CRC64/SHA-256/no check |
| .lzma | `lzma` | 1-9 (6) | as xz | unknown size with end marker |
| zstd (RFC 8878) | `zstd` | 1-19 (3) | hash chains with repeat offsets; Huffman literals; predefined/RLE/FSE tables | dictionaries (`-D`), skippable frames, all block and table modes |

The library also contains teaching codecs that are not file formats:
static and adaptive arithmetic coding (`arith.c`), rANS and interleaved
rANS (`rans.c`), and an order-0 FSE codec (`fse.c`).

## Build and test

```
make              # build/libsquash.a and build/squash
make test         # unit, round-trip, robustness and interop tests
make SAN=1 test   # the same under AddressSanitizer + UBSan (build-san/)
```

Interop tests use whichever official tools are installed and report the
rest as `SKIPPED`: `gzip`, Python's `zlib` module (zlib and raw DEFLATE),
`bzip2`, `lz4`, `xz`, `zstd`, `compress` (ncompress) and python-snappy. Point `PYTHON`
at an interpreter that has python-snappy, and put `compress` on `PATH`:

```
PATH=/path/to/ncompress:$PATH PYTHON=/path/to/venv/bin/python make test
```

## Benchmark and fuzzing

```
make bench                                   # generated text, binary, random data
scripts/fetch-silesia.sh                     # Silesia corpus into bench/silesia
make bench BENCH_FILES="bench/silesia/*"
build/squash bench -a FILE...                # also variants, fastest/strongest levels
make fuzz FUZZ_TIME=300                      # libFuzzer over every decoder (clang)
```

`squash bench` prints compression ratio and MB/s (compress, decompress)
for each codec at its default level, then the same for the installed
reference tools (`gzip`, `bzip2`, `lz4`, `xz`, `zstd`, `compress`), which
are timed as processes and so include start-up and file I/O. `-a` adds
the container variants (zlib, raw DEFLATE, .lzma, raw Snappy), every
codec's fastest and strongest level, and `gzip -9`, `lz4 -9`, `zstd -19`.

Expect the full Silesia corpus (211 MB) to take around 7 minutes by
default and well over half an hour with `-a`: the slow entries are the
LZMA-based encoders (ours and `xz`, 1-3 MB/s) and bzip2, and every input
is compressed and decompressed at least once per codec.

## CLI

```
squash [-f FORMAT] [-d] [-l LEVEL] [-c] [-F] [-D DICT] [IN [OUT]]
squash -L                       # list formats, suffixes and level ranges
squash -d -D dict file.zst      # zstd with a dictionary
squash bench [-a] [FILES...]    # ratio and speed table
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
| `src/bench.c` | `squash bench`: ratio and speed against the system tools |
| `scripts/fetch-silesia.sh` | downloads the Silesia corpus for benchmarking |
| `fuzz/fuzz_decode.c`, `fuzz/seed.sh` | libFuzzer target over all decoders, and its seed corpus |
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
| `src/huf0.[ch]` | zstd Huffman: weights, tree description, 1/4-stream coding |
| `src/zstd.h` | zstd constants and shared declarations |
| `src/zstd_tables.c` | code baselines, default distributions, repeat-offset rules |
| `src/zstd_dec.c` | zstd decoder (frames, blocks, literals, sequences, dictionaries) |
| `src/zstd_enc.c` | zstd encoder |
| `src/lzma_price.c` | LZMA decision prices from the current model |
| `src/lzma_opt.c` | LZMA optimal parser (levels 7-9) |
| `tests/test.h` | tiny test harness |
| `tests/test_*.c` | one unit test per module |
| `tests/corpus.h` | generated inputs: empty, 1 byte, zeros, random, text, binary, boundary sizes, >1 block |
| `tests/test_roundtrip.c` | every codec and level: compress, decompress, compare |
| `tests/test_robust.c` | truncated, bit-flipped and garbage input must fail cleanly |
