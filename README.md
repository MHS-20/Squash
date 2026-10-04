# Squash

A compression library (`libsquash`) and command-line tool (`squash`) in C11,
written from scratch from the format specifications.

## Build and test

```
make              # build/libsquash.a and build/squash
make test         # unit, round-trip, robustness and interop tests
make SAN=1 test   # the same under AddressSanitizer + UBSan (build-san/)
```

## Layout

| File | Concept |
|---|---|
| `src/squash.h` | public API and error codes |
| `src/buf.[ch]` | growable output buffer, shared LZ77 back-reference copy |
| `src/bitio.[ch]` | LSB-first and MSB-first bit readers and writers |
| `src/checksum.h` | checksum API |
| `src/crc32.c` | CRC-32, CRC-32C, bzip2 CRC (table driven) |
| `src/adler32.c` | Adler-32 |
| `src/xxhash.c` | xxHash32 / xxHash64 |
| `tests/test.h` | tiny test harness |
| `tests/test_*.c` | one unit test per module |
