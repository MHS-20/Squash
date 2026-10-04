#!/bin/sh
# Downloads the Silesia corpus (about 68 MB zipped, 211 MB unpacked) into
# bench/silesia for `make bench BENCH_FILES="bench/silesia/*"`.
set -eu
dir=$(dirname "$0")/../bench/silesia
mkdir -p "$dir"
url=https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
curl -fsSL -o "$dir/silesia.zip" "$url"
if command -v unzip >/dev/null 2>&1; then
    unzip -q -o "$dir/silesia.zip" -d "$dir"
else
    python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
        "$dir/silesia.zip" "$dir"
fi
rm "$dir/silesia.zip"
ls -l "$dir"
