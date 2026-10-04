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

## LZ77 match finding

- One parser (`lz_parse`) produces `(literal run, match length, distance)`
  sequences for every LZ77 format; each format only supplies limits
  (window, min/max length, LZ4's end-of-block margins).
- Hash chains as in zlib: `head[hash]` holds the newest position with that
  hash and `prev[pos % window]` links older ones. The chain is cut by
  `max_chain` (speed/ratio knob) and by `nice_len` (good enough, stop).
- Lazy matching is zlib's `deflate_slow` idea with one step of lookahead: a
  match shorter than `lazy` is held back one byte, and dropped for a literal
  if the next position matches longer. Greedy is `lazy = 0`.
- `lz_parse_naive` is the brute-force baseline. With an unbounded chain the
  greedy hash-chain parse is identical to it, which the unit test checks.
- Every position, including those inside matches, is inserted. zlib skips
  insertion inside long matches at fast levels; that is a speed trick only.

## DEFLATE, zlib, gzip

- Decoding writes straight into the output buffer, which also serves as the
  32 KB window; a distance is checked against the bytes produced so far
  (`sq_buf_lzcopy`). Each code is one probe into the 2^15-entry table.
- Malformed streams are caught at the point of use: reserved block type and
  symbols (286/287, distance 30/31), bad stored LEN/NLEN, repeat code 16
  with nothing to repeat, a repeat running past HLIT+HDIST, a missing
  end-of-block code, over-subscribed lengths, distances before the start,
  and any read past the end of input.
- The encoder parses with `lz_parse` using zlib's per-level
  `{nice, lazy, chain}` values (levels 1-3 greedy, 4-9 lazy), then cuts the
  parse into blocks of 16384 symbols. Each block is costed three ways —
  stored, fixed, dynamic — and the cheapest is written, as libdeflate does.
  Block boundaries are fixed-size, not chosen from statistics (no block
  splitting); that is the main ratio gap to zlib -9 and libdeflate.
- Like zlib, the encoder always gives the distance code at least two
  symbols, since some inflaters reject a one-symbol distance code.
- gzip writes MTIME = 0 and no name, so output is reproducible. The decoder
  handles every header flag (FEXTRA, FNAME, FCOMMENT, FHCRC) and multiple
  members; zlib streams with a preset dictionary are reported as
  unsupported.
- `src/codec.c` is a small registry (name, suffix, magic, level range,
  function pointers) used by the CLI and by the generic round-trip and
  robustness tests, so a new format is tested by adding one line.

## LZW (.Z)

- There is no written spec; ncompress's `compress.c` is the format. Three
  details decide compatibility:
  1. **Widening.** The decoder learns each dictionary entry one code after
     the encoder made it, so the encoder widens when its next free code
     exceeds 2^n and the decoder when its own exceeds 2^n - 1.
  2. **Group padding.** compress writes codes in groups of eight; on a
     width change or after CLEAR, the rest of the group is skipped, so the
     position is rounded up to a multiple of `n_bits * 8` bits measured
     from where the previous width began.
  3. **After CLEAR** the decoder resumes at free code 256 (not 257): the
     next code harmlessly defines slot 256, keeping both sides in step.
- The encoder uses compress's reset policy: once the table is full it checks
  the compression ratio every 10000 input bytes and emits CLEAR when the
  ratio drops. Without a reset its output is byte-identical to ncompress
  (checked in `tests/test_lzw.c`); with resets it differs only in where the
  checks land.
- `level` is the maximum code width (9-16), like `compress -b`.
- Decoder errors: width outside 9-16, a first code above 255, and a code
  beyond the next free entry (only `code == next` is the legal KwKwK case).
- `gzip -d` can also read `.Z` files but rejects valid `-b 9` streams from
  ncompress itself, so interop uses it only for the default width.

## LZ4

- Block encoding reuses `lz_parse` with LZ4's limits: 4-byte minimum,
  65535 maximum offset, unbounded length, and the end-of-block rules (last
  5 bytes literal, last match starting at least 12 bytes before the end)
  expressed as `end_margin` / `start_margin`. Levels 1-5 are greedy with
  chains of 1-16; 6-9 are lazy with longer chains, standing in for LZ4-HC.
  LZ4-HC's real optimal parser is not implemented.
- The frame encoder writes independent 4 MB blocks with content size and
  content checksum, and stores a block raw when compression does not shrink
  it, as the spec requires.
- The decoder accepts everything `lz4` writes: linked or independent
  blocks, block checksums, every block size, content size (verified),
  skippable frames, legacy frames, and concatenated frames. Dictionary IDs
  are reported as unsupported.
- The decoder is lenient about the end-of-block rules (the spec allows
  rejecting violations but does not require it) and strict about bounds:
  every literal run, offset and match is checked against the input, the
  bytes already produced, and the block's maximum size.

## Snappy

- The raw encoder works in independent 64 KB fragments like the reference
  and parses each with `lz_parse` (greedy, chain of 4, 4-byte minimum).
  The reference uses a single-probe hash table with a skip heuristic that
  speeds through incompressible data; the chain is simpler and finds
  slightly more.
- Copies longer than 64 are split exactly as the reference `EmitCopy` does:
  64-byte pieces while at least 68 remain, so the tail stays >= 4 and can
  use the compact 1-byte-offset form when short and near.
- The framing encoder writes 64 KB chunks and falls back to an uncompressed
  chunk unless compression saves at least 1/8.
- The framing decoder verifies every masked CRC-32C, skips padding and
  reserved skippable chunks, rejects reserved unskippable ones, and accepts
  repeated stream identifiers (concatenated streams). An empty input is
  accepted as an empty stream, because the reference tools write nothing
  for empty input.

## Upstream references (opened and verified)

Pinned commits:

| Repo | Commit |
|---|---|
| madler/zlib | 767c4c947852e143f582c85f14cf573411df1b35 |
| Cyan4973/xxHash | 680bf463fa1ca0461b9a7c2dab7556e1f54cf4cf |
| libarchive/bzip2 | 1ea1ac188ad4b9cb662e3f8314673c63df95a589 |
| ebiggers/libdeflate | 92e6a0db9fa848d742f9eb286c92afc60f2c3dda |
| richgel999/miniz | 2948eebafc5b9f1c69bebed80333e403a09bd62d |
| vapier/ncompress | 03592d7a4702bb83558b6f1520c2a6a5381374c8 |
| lz4/lz4 | 0774d05537f9762f838f7ab541b7765f1a729cb5 |
| google/snappy | 9c28114a38866f6deeaa826db918293bc28ae410 |

| Module | Upstream file:line | What it informed |
|---|---|---|
| CRC-32 | zlib `crc32.c:157`, `crc32.c:175` | reflected polynomial and the byte-table recurrence |
| Adler-32 | zlib `adler32.c:10-12` | modulus and NMAX bound |
| xxHash32 | xxHash `xxhash.h:2922-2926`, `2947-2951`, `3004-3010`, `3187-3203` | primes, lane round, avalanche, short-input path |
| xxHash64 | xxHash `xxhash.h:3481-3485`, `3496`, `3521-3525` | primes, round and merge round |
| Huffman | zlib `trees.c:540-593` (`gen_bitlen` overflow fix), `trees.c:203-227` (`gen_codes`) | canonical code assignment; zlib's alternative to flattening |
| Huffman | zlib `inftrees.c:139-147` | over-subscribed / incomplete checks |
| Huffman | bzip2 `huffman.c:63-147` (`BZ2_hbMakeCodeLengths`) | length limiting by weight halving |
| LZ77 | zlib `deflate.c:112-123` (`configuration_table`) | good/lazy/nice/chain knobs per level |
| LZ77 | zlib `deflate.c:1390-1425` (`longest_match`) | chain walk, distance limit, chain shortening |
| LZ77 | zlib `deflate.c:1957-2077` (`deflate_slow`) | one-step lazy evaluation |
| DEFLATE decode | zlib `inflate.c:747-760` (stored LEN/NLEN), `782-800` (HLIT/HDIST limits), `825-875` (repeat codes), `1012` (distance too far) | validation points |
| zlib header | zlib `inflate.c:529-559` | FCHECK, method and window checks |
| zlib header | miniz `miniz_tinfl.c:234` | the same three header checks in one expression |
| DEFLATE decode | miniz `miniz_tinfl.c:289`, `536` | reserved block type, distance-before-start check |
| DEFLATE encode | libdeflate `lib/deflate_compress.c:1706-2038` (`deflate_flush_block`) | choosing stored / static / dynamic by exact bit cost |
| LZW | ncompress `compress.c:217`, `232-248` | `MAXCODE`, encoder/decoder reset of width and limits |
| LZW | ncompress `compress.c:1064-1074` | encoder widening and group padding |
| LZW | ncompress `compress.c:1082-1108` | ratio check and CLEAR |
| LZW | ncompress `compress.c:1345-1356`, `1373-1380` | decoder widening, CLEAR handling and padding |
| LZW | ncompress `compress.c:1386-1393`, `1449-1453` | KwKwK case, entry creation limit |
| LZ4 block | lz4 `doc/lz4_Block_format.md:110-130` | end-of-block conditions |
| LZ4 block | lz4 `lib/lz4.c:243-264` | `MINMATCH`, `LASTLITERALS`, `MFLIMIT`, token nibble masks |
| LZ4 frame | lz4 `doc/lz4_Frame_format.md:126-195`, `240-300`, `333`, `360-400` | descriptor flags, block size field, skippable and legacy frames |
| LZ4 frame | lz4 `lib/lz4frame.c:351`, `1444` | header checksum = second byte of XXH32 of the descriptor |
| Snappy raw | snappy `format_description.txt:15-30`, `47-60`, `90-110` | varint preamble, literal and copy tag layouts |
| Snappy raw | snappy `snappy.cc:619`, `671-730` (`EmitLiteral`, `EmitCopyAtMost64`, `EmitCopy`) | element emission and splitting of long copies |
| Snappy framing | snappy `framing_format.txt:40-56`, `68-132` | masked CRC-32C, chunk types |
