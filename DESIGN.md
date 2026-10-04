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

## BWT, SA-IS, MTF, RLE

- `bwt_naive` sorts rotations with `qsort` — the definition, kept as the
  oracle the tests compare against. `bwt_forward` gets the same order in
  linear time: the rotations of S are the length-n prefixes of the
  suffixes of SS that start in the first half, so it builds the suffix
  array of SS with SA-IS and keeps the entries below n. Doubling costs 2x
  memory; bzip2 instead sorts rotations directly (`mainSort` with a
  `fallbackSort` for repetitive data), and libsais has a dedicated BWT
  entry point. Doubling keeps the suffix sorter generic and the BWT a few
  lines.
- `sais.c` is a plain recursive SA-IS: classify S/L types, place LMS
  suffixes, induce, name LMS substrings, recurse if names repeat, induce
  again. Types are a byte array and buckets are recomputed per pass;
  libsais's in-place tricks and parallel scans are left out.
- The inverse BWT is bzip2's: build `next[]` from cumulative counts and
  follow it from the primary row.
- MTF runs over the full 0..255 list; bzip2 first maps bytes to the
  dense range of bytes in use so the list is short.
- RLE1 is resumable with an output cap so the bzip2 encoder can fill a
  block to exactly its size limit without splitting a run. RLE2's decoder
  bounds the run digit weight like bzip2 (`N < 2M`), here against the
  block size, so a long RUNA/RUNB string is an error, not an overflow.

## bzip2

- The encoder is the bzip2 pipeline with the transforms from the modules
  above: RLE1 fills a block to `level * 100000 - 19` bytes (bzip2's own
  limit), then BWT, MTF over the dense alphabet of bytes in use, RLE2, and
  Huffman coding.
- Table selection follows bzip2: 2-6 tables depending on the symbol count,
  started from contiguous equal-frequency ranges of the alphabet, then four
  passes of "give each 50-symbol group its cheapest table, rebuild each
  table from its groups". Code lengths are capped at 17 like bzip2 >= 1.0.3.
  bzip2 additionally nudges the initial range boundaries and has faster
  cost loops; output size is within a fraction of a percent of `bzip2 -9`.
- The decoder checks every field bzip2 checks: table count 2-6, at least
  one selector, unary selector indices below the table count, code lengths
  1-20, origPtr inside the block, RUNA/RUNB run bounds, block size against
  the header digit, block CRC and combined CRC. Selectors beyond 18002 are
  read and ignored, as bzip2 1.0.8 does. Over-subscribed code lengths are
  rejected (bzip2 does not check; no real encoder produces them).
- Randomised blocks (written by bzip2 before 0.9.5) are decoded by
  toggling the pattern from bzip2's fixed 512-entry table. The encoder can
  produce them for tests only (`bzip2_compress_ex`), and `tests/test_bzip2.c`
  checks that the real `bzip2` decodes them.
- Concatenated streams decode in order, like `bzip2 -d`.

## Arithmetic coding and the range coder

- `arith.c` is the Witten-Neal-Cleary coder with 32-bit code values held in
  64-bit integers, so `range * cumulative` never overflows. Frequencies are
  capped at 2^16 total, which guarantees every symbol a non-empty slice of
  the narrowest possible interval. Both models are order 0; the adaptive one
  adds 32 per occurrence and halves all counts at the cap. Cumulative
  frequencies are found by a linear scan — a Fenwick tree would make it
  O(log n), at the cost of a second data structure to explain.
- These are teaching codecs with a minimal ad-hoc container (length +
  frequencies), not file formats, so they are not in the CLI.
- `rangecoder.c` is LZMA's binary range coder: 11-bit probabilities with
  shift-by-5 adaptation, normalisation at 2^24, and carry handling through
  a cached byte plus a count of pending 0xFF bytes (`shift_low`). It adds
  bit-tree helpers in both bit orders and a floating-point bit price used
  by encoders (LZMA SDK uses a precomputed price table).

## LZMA, LZMA2, xz

- One probability model (`lzma.h`, `lzma_model.c`) serves both directions.
  The decoder follows the structure of the LZMA SDK's `LZMA_DECODE_REAL`
  but writes the matched-literal loop the way the LZMA specification
  explains it (code bits against the byte at rep0 until they disagree).
- The output buffer is the dictionary: back-references are checked against
  the bytes produced since the last dictionary reset, and positions for
  `pb`/`lp` contexts count from that reset, as in the SDK.
- Encoder: the LZMA SDK's fast mode, re-derived. At each position try the
  four repeat distances and the hash-chain match; prefer a repeat that is
  nearly as long (the SDK's thresholds), take one step of lazy evaluation,
  and choose between a literal and a "short rep" by bit price. Distances
  that equal a recent one are coded as repeats. The SDK's optimal parser
  (pricing paths of up to 4096 decisions) is not implemented; that is
  where most of the gap to `xz -6` (about 15% on source code) comes from.
- Levels 1-9 set dictionary size (1-16 MiB, shrunk to the input), chain
  length and nice length. lc=3, lp=0, pb=2.
- LZMA2 chunks hold at most 2 MiB in and 64 KiB out; the encoder stops a
  chunk when another operation might not fit, and stores a chunk that did
  not shrink. After a stored chunk the next LZMA chunk resets the state
  (and, after a stored first chunk, sends properties) — the same rule
  liblzma's encoder follows.
- Decoders demand exact framing: an LZMA2 chunk's range coder must end
  exactly at its compressed size with `code == 0`; a .lzma stream with
  unknown size must end with the end marker; matches may not cross the
  declared end.
- xz: one block per stream with CRC64 when encoding. The decoder handles
  multiple blocks and streams, stream padding, optional block sizes, all
  CRC-protected fields, and None/CRC32/CRC64/SHA-256 checks. Filters other
  than a lone LZMA2 (BCJ, delta) are reported as unsupported.
- `crc64.c` and `sha256.c` provide the two extra xz checks.

## ANS: rANS, tANS/FSE

- `rans.c` follows ryg_rans's byte-wise coder: state in [2^23, 2^31),
  renormalised one byte at a time, 12-bit frequencies, encoder running
  backwards into a buffer filled from the end. The interleaved variant
  alternates two states over the symbols; the format work is identical,
  but the two dependency chains can overlap in a CPU pipeline.
- `fse.c` is tANS exactly as zstd needs it: the symbol spread (step
  `size/2 + size/8 + 3`, "less than one" symbols at the top), decoding
  tables of (symbol, bits, base), encoding tables of next states with
  zstd's `deltaNbBits`/`deltaFindState` per symbol, the table description
  format (variable-width counts with 2-bit zero-run flags), and a backward
  bit reader in `bitio.h`.
- Normalisation is not zstd's: rounded proportional shares, at least one
  state per present symbol, -1 for symbols below one state (when allowed),
  and the rounding error settled on the largest symbol. zstd's
  `FSE_normalizeCount` uses fixed-point thresholds and a fallback method
  (`FSE_normalizeM2`) for skewed inputs and does slightly better; any
  normalisation that sums correctly is valid for the format. rANS reuses it
  with the -1 option off.
- The unit test checks the decoding table built from RFC 8878's predefined
  literal-length distribution against the state listing in the RFC.
- Both are teaching codecs with ad-hoc containers, kept out of the CLI.

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
| IlyaGrebnov/libsais | b6e52ef33fe14f9d5c14c580d162b6fd2c27f2a8 |
| ip7z/7zip (LZMA SDK) | 0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17 |
| tukaani-project/xz | 3b1efb04d17c3a9ef7f473d73af13f1531428ffe |
| rygorous/ryg_rans | c9d162d996fd600315af9ae8eb89d832576cb32d |
| facebook/zstd | 01b7154f1172432f8abe9b3bb9909e14a1176b7d |

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
| BWT | bzip2 `blocksort.c:1031-1089` (`BZ2_blockSort`) | rotation sort entry point and how `origPtr` is found |
| BWT | bzip2 `blocksort.c:212` (`fallbackSort`) | the alternative for highly repetitive blocks |
| SA-IS | libsais `src/libsais.c:459`, `726` | LMS gathering and bucket counting |
| SA-IS | libsais `src/libsais.c:2157`, `2989` | left-to-right (L) and right-to-left (S) induction scans |
| SA-IS | libsais `src/libsais.c:4103`, `6668`, `7097` | LMS renaming, recursion, BWT entry point |
| RLE2 | bzip2 `compress.c:171-187` | zero runs as RUNA/RUNB, bijective base 2 |
| RLE2 | bzip2 `decompress.c:385-399` | run decoding and its overflow guard |
| bzip2 | bzip2 `decompress.c:280-345` | symbol map, selectors (MTF + unary, `BZ_MAX_SELECTORS` clamp), delta-coded lengths |
| bzip2 | bzip2 `decompress.c:358`, `470-525` | block size bound, origPtr and cftab checks |
| bzip2 | bzip2 `bzlib_private.h:114-123`, `135-147` | group size, table count, selector limit, randomisation mask |
| bzip2 | bzip2 `randtable.c` (`BZ2_rNums`) | the randomisation table (format data) |
| bzip2 | bzip2 `compress.c:235-315` | table count by symbol count, initial equal-frequency partition |
| bzip2 | bzip2 `compress.c:450` | lengths rebuilt per iteration, limit 17 |
| bzip2 | bzip2 `bzlib.c:196` | `nblockMAX = 100000 * level - 19` |
| Range coder | 7-Zip `C/LzmaDec.c:11-26` | `kTopValue`, 11-bit probabilities, move bits, `NORMALIZE`, bit decode/update |
| Range coder | 7-Zip `C/LzmaEnc.c:691-733` (`RangeEnc_ShiftLow`, `RangeEnc_FlushData`) | carry propagation through the cache byte, 5-byte flush |
| LZMA decode | 7-Zip `C/LzmaDec.c:91-118` | model constants (length coder, slots, align bits, min match) |
| LZMA decode | 7-Zip `C/LzmaDec.c:234-613` (`LZMA_DECODE_REAL`) | decode loop, matched literals (`291`), end marker (`525`) |
| LZMA2 decode | 7-Zip `C/LzmaDec.c:911-922`, `C/Lzma2Dec.c:238-262` | what dictionary / state resets reset (position counter) |
| LZMA encode | 7-Zip `C/LzmaEnc.c:1977-2100` (`GetOptimumFast`) | rep-vs-match thresholds, lazy step |
| LZMA2 decode | xz `src/liblzma/lzma/lzma2_decoder.c:56-200` | control byte semantics, property and dictionary reset rules |
| LZMA2 encode | xz `src/liblzma/lzma/lzma2_encoder.c:200-212` | storing chunks that did not compress, then a state reset |
| LZMA props | xz `src/liblzma/lzma/lzma_decoder.c:1199` | lc/lp/pb byte |
| xz | xz `doc/xz-file-format.txt:184-250`, `336-470`, `498-660`, `647-800`, `845-900` | varints, stream header/footer, block header, index, LZMA2 dictionary byte |
| rANS | ryg_rans `rans_byte.h:50-140` | byte-wise rANS: L, x_max renormalisation, put/flush/init/advance |
| rANS | ryg_rans `main.cpp:216-252` | two-way interleaving and flush order |
| FSE | zstd `lib/common/entropy_common.c:43-190` (`FSE_readNCount_body`) | table description decoding, zero-run flags |
| FSE | zstd `lib/compress/fse_compress.c` (`FSE_buildCTable_wksp`, `FSE_writeNCount`, `FSE_normalizeCount:465`) | encoding table, `deltaNbBits`, description writer, zstd's normaliser |
| FSE | zstd `lib/common/fse_decompress.c:59` (`FSE_buildDTable_internal`) | decoding table construction |
| FSE | zstd `lib/common/fse.h:443-466`, `623` | encoder state init/encode/flush, `FSE_TABLESTEP` |
