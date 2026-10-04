#!/bin/sh
# Interop with the official tools, both directions:
#   our output    -> official decompressor
#   official output (several levels) -> our decompressor
# A format whose tool is missing is reported as SKIPPED, not failed.
# PYTHON may point at an interpreter with python-snappy installed.
set -u
SQ=${1:?usage: interop.sh path/to/squash}
PY=${PYTHON:-python3}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0

# ---- inputs -------------------------------------------------------------
: > "$T/empty"
printf x > "$T/one"
head -c 1000000 /dev/zero > "$T/zeros"
head -c 300000 /dev/urandom > "$T/random"
src_dir=$(dirname "$0")/../src
i=0; : > "$T/text"
while [ $i -lt 12 ]; do cat "$src_dir"/*.c >> "$T/text"; i=$((i + 1)); done
{ head -c 1500000 "$T/text"; head -c 500000 /dev/urandom; head -c 1500000 /dev/zero; } > "$T/large"
INPUTS="empty one zeros random text large"

have() { command -v "$1" >/dev/null 2>&1; }
pyhas() { "$PY" -c "import $1" >/dev/null 2>&1; }

# run FORMAT LABEL COMMAND...: COMMAND turns $IN into $T/out; compares.
result() {
    if [ "$2" = ok ]; then pass=$((pass + 1)); else
        fail=$((fail + 1)); echo "  FAIL $1"; fi
}

# ours_to FMT DECODE-CMD: DECODE-CMD reads stdin, writes stdout.
ours_to() {
    fmt=$1; shift
    for f in $INPUTS; do
        if "$SQ" -f "$fmt" -c "$T/$f" > "$T/z" && "$@" < "$T/z" > "$T/o" 2>/dev/null &&
           cmp -s "$T/o" "$T/$f"; then result "squash->tool $fmt $f" ok
        else result "squash->tool $fmt $f" bad; fi
    done
}

# tool_to FMT ENCODE-CMD: ENCODE-CMD reads stdin, writes stdout.
tool_to() {
    fmt=$1; shift
    for f in $INPUTS; do
        if "$@" < "$T/$f" > "$T/z" 2>/dev/null && "$SQ" -d -f "$fmt" -c "$T/z" > "$T/o" &&
           cmp -s "$T/o" "$T/$f"; then result "tool->squash $fmt $f ($*)" ok
        else result "tool->squash $fmt $f ($*)" bad; fi
    done
}

report() {
    if [ "$fail" -eq 0 ]; then printf '%-14s PASS  (%d cases)\n' "interop $1" "$pass"
    else printf '%-14s FAIL  (%d of %d cases)\n' "interop $1" "$fail" $((pass + fail)); fails=1; fi
}

skip() { printf '%-14s SKIPPED  (%s not found)\n' "interop $1" "$2"; }

# ---- gzip -----------------------------------------------------------------
if have gzip; then
    pass=0 fail=0
    ours_to gzip gzip -dc
    for l in 1 6 9; do tool_to gzip gzip -c -$l; done
    report gzip
else skip gzip gzip; fi

# ---- zlib and raw deflate (via Python's zlib module, i.e. libz) -----------
zpy() { "$PY" -c "import sys,zlib; d=sys.stdin.buffer.read(); $1" ; }
if pyhas zlib; then
    pass=0 fail=0
    ours_to zlib zpy 'sys.stdout.buffer.write(zlib.decompress(d))'
    for l in 1 6 9; do tool_to zlib zpy "sys.stdout.buffer.write(zlib.compress(d, $l))"; done
    report zlib
    pass=0 fail=0
    ours_to deflate zpy 'sys.stdout.buffer.write(zlib.decompress(d, -15))'
    for l in 1 9; do
        tool_to deflate zpy "c=zlib.compressobj($l, zlib.DEFLATED, -15); sys.stdout.buffer.write(c.compress(d)+c.flush())"
    done
    report deflate
else skip zlib "python3 zlib"; skip deflate "python3 zlib"; fi

# ---- LZW (.Z) ---------------------------------------------------------------
if have compress; then
    pass=0 fail=0
    ours_to lzw compress -dc
    # compress exits 2 when the output is not smaller; that is not an error.
    compress_c() { compress -c "$@"; [ $? -le 2 ]; }
    for b in 9 12 16; do tool_to lzw compress_c -b $b; done
    # gzip also reads .Z files (it fails on -b9 streams, so default width only).
    if have gzip; then ours_to lzw gzip -dc; fi
    report lzw
else skip lzw "compress (ncompress)"; fi

# ---- bzip2 --------------------------------------------------------------------
if have bzip2; then
    pass=0 fail=0
    ours_to bzip2 bzip2 -dc
    for l in 1 6 9; do tool_to bzip2 bzip2 -c -$l; done
    report bzip2
else skip bzip2 bzip2; fi

# ---- LZ4 frame --------------------------------------------------------------
if have lz4; then
    pass=0 fail=0
    ours_to lz4 lz4 -dc
    for opt in -1 -9 -12 "-1 -BD -B4" "-9 -BX -B5" "-1 --no-frame-crc --content-size" "-1 -l"; do
        # shellcheck disable=SC2086
        tool_to lz4 lz4 -c $opt
    done
    report lz4
else skip lz4 lz4; fi

# ---- Snappy (python-snappy; recent versions are backed by cramjam) ----------
if pyhas snappy; then
    pass=0 fail=0
    spy() { "$PY" -c "import sys,snappy; d=sys.stdin.buffer.read(); $1"; }
    # The module's own CLI is broken for -d, so drive the stream classes.
    ours_to snappy spy 'sys.stdout.buffer.write(snappy.StreamDecompressor().decompress(d))'
    tool_to snappy spy 'sys.stdout.buffer.write(snappy.StreamCompressor().add_chunk(d))'
    ours_to snappy-raw spy 'sys.stdout.buffer.write(snappy.decompress(d))'
    tool_to snappy-raw spy 'sys.stdout.buffer.write(snappy.compress(d))'
    report snappy
else skip snappy "python-snappy"; fi

exit $fails
