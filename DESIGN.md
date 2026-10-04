# Design notes

## Conventions

- Whole-buffer API: every codec reads an input buffer and appends to an
  `sq_buf`. This keeps each format's logic free of streaming state machines.
  The output is capped at `SQ_BUF_MAX` so a hostile stream cannot demand
  unbounded memory.
- Decoders never trust input. Bit readers return zero bits past the end and
  raise an `overrun` flag that the decoder checks, so truncation is an error
  rather than an out-of-bounds read.

## Bit I/O and checksums

- Readers hold a 64-bit buffer and refill a byte at a time; a peek of up to
  32 bits is always satisfiable, which is what table-driven Huffman decoding
  needs.
- CRCs use one 256-entry table per polynomial, built once with `call_once`.
  zlib uses slicing-by-N and braided CRCs for speed; one table is the
  readable baseline.
- Adler-32 defers the modulo for `NMAX = 5552` bytes, exactly as zlib does.

## Huffman

- Lengths come from the two-queue construction (sorted leaves plus a queue
  of merged nodes that is sorted by construction). Length limiting uses
  bzip2's flattening heuristic: halve the weights and rebuild. Package-merge
  is optimal but much longer; the loss from flattening is negligible.
- Decoding has two forms. DEFLATE uses a single-level lookup table of
  2^15 entries indexed by the next 15 bits (one probe per symbol); zlib and
  libdeflate use a smaller root table plus sub-tables, which is faster to
  build but harder to read. bzip2 codes go up to 20 bits, where a full table
  is too big, so it uses the canonical count/symbol walk.
- Over-subscribed length sets are rejected. Incomplete sets are accepted,
  but unused bit patterns decode to an error rather than a symbol.

## Upstream references (opened and verified)

Pinned commits:

| Repo | Commit |
|---|---|
| madler/zlib | 767c4c947852e143f582c85f14cf573411df1b35 |
| Cyan4973/xxHash | 680bf463fa1ca0461b9a7c2dab7556e1f54cf4cf |
| libarchive/bzip2 | 1ea1ac188ad4b9cb662e3f8314673c63df95a589 |

| Module | Upstream file:line | What it informed |
|---|---|---|
| CRC-32 | zlib `crc32.c:157`, `crc32.c:175` | reflected polynomial and the byte-table recurrence |
| Adler-32 | zlib `adler32.c:10-12` | modulus and NMAX bound |
| xxHash32 | xxHash `xxhash.h:2922-2926`, `2947-2951`, `3004-3010`, `3187-3203` | primes, lane round, avalanche, short-input path |
| xxHash64 | xxHash `xxhash.h:3481-3485`, `3496`, `3521-3525` | primes, round and merge round |
| Huffman | zlib `trees.c:540-593` (`gen_bitlen` overflow fix), `trees.c:203-227` (`gen_codes`) | canonical code assignment; zlib's alternative to flattening |
| Huffman | zlib `inftrees.c:139-147` | over-subscribed / incomplete checks |
| Huffman | bzip2 `huffman.c:63-147` (`BZ2_hbMakeCodeLengths`) | length limiting by weight halving |
