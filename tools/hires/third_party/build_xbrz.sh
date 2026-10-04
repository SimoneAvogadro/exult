#!/usr/bin/env bash
# Build libxbrz19.so (xBRZ 1.9, GPLv3) for the offline art tools (DESIGN §8.1).
# xBRZ is never vendored: the zip is fetched, checked against its SHA-256, extracted with
# "python3 -m zipfile -e" (unzip is not installed), patched for g++ 9 (a C++23 lambda without a
# parameter list, xbrz.cpp:419) and built with our C wrapper. The patched-source SHA-256 is printed
# and written next to the library; the route sidecars record it.
#
# usage: build_xbrz.sh [OUT_DIR] [ZIP]     (default OUT_DIR = /home/simonea/ultima7_exult/tmp/algo/xbrz19)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/home/simonea/ultima7_exult/tmp/algo/xbrz19}"
ZIP="${2:-}"
URL="https://sourceforge.net/projects/xbrz/files/xBRZ/xBRZ_1.9.zip/download"
ZIP_SHA="b2dff73b3abd24a18a7cde78d5ff5ed8f0922296dce6ed734dce2264cd0a0fc9"
PATCHED_SHA="247efab3a99b21ba635df6f991d26970309f917bfaec59d3629404c3f8187f76"

mkdir -p "$OUT"
WORK="$(mktemp -d "$OUT/build.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
if [[ -z "$ZIP" ]]; then
    ZIP="$WORK/xBRZ_1.9.zip"
    curl -sSL -o "$ZIP" "$URL"
fi
echo "$ZIP_SHA  $ZIP" | sha256sum -c -
python3 -m zipfile -e "$ZIP" "$WORK/src"
cd "$WORK/src"
patch -p1 < "$HERE/xbrz-1.9-gcc9.patch"
echo "$PATCHED_SHA  xbrz.cpp" | sha256sum -c -
cp "$HERE/xbrz_cwrap.cpp" .
g++ -O2 -std=c++2a -shared -fPIC -o libxbrz19.so xbrz.cpp xbrz_cwrap.cpp
cp libxbrz19.so xbrz.cpp xbrz.h xbrz_config.h xbrz_tools.h License.txt Changelog.txt xbrz_cwrap.cpp "$OUT/"
echo "$PATCHED_SHA" > "$OUT/xbrz_patched_source.sha256"
echo "built $OUT/libxbrz19.so (patched source sha256 $PATCHED_SHA)"
