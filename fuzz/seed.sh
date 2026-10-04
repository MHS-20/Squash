#!/bin/sh
# Seeds the fuzzing corpus with one valid stream per format, prefixed by
# the format's index in the registry (the byte the fuzz target reads).
set -eu
SQ=$1 DIR=$2
i=0
for f in $("$SQ" -L | awk '{print $1}'); do
    for src in "$(dirname "$0")/../README.md" "$(dirname "$0")/../src/lz77.c"; do
        { printf "\\$(printf %o "$i")"; "$SQ" -f "$f" -c "$src"; } > "$DIR/seed-$f-$(basename "$src")"
    done
    i=$((i + 1))
done
