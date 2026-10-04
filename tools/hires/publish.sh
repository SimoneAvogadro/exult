#!/usr/bin/env bash
# Mirror a pack from the canonical ext4 copy to the Windows side (DESIGN §5.7, §13 Q7):
#   /home/simonea/ultima7_exult/packs/<name>  ->  /mnt/e/Dati/Ultima7_Upscale/packs/<name>
# then touch x6/.reload on both sides so a running dev-mode engine reloads.
#
# usage: publish.sh [--delete] [--fast] [--dry-run] <name> [scale]
#   --delete   also delete files on E: that no longer exist on ext4 (default: keep them)
#   --fast     compare size+mtime instead of checksums (default: --checksum, as in the design)
#   --dry-run  show what would change
# env: U7_PACKS_SRC (default /home/simonea/ultima7_exult/packs), U7_PACKS_DST (default /mnt/e/Dati/Ultima7_Upscale/packs)
set -euo pipefail
SRC_ROOT="${U7_PACKS_SRC:-/home/simonea/ultima7_exult/packs}"
DST_ROOT="${U7_PACKS_DST:-/mnt/e/Dati/Ultima7_Upscale/packs}"
OPTS=(-rt --checksum --itemize-changes --exclude '*.tmp' --exclude '.reload')
DRY=0
while [[ $# -gt 0 && "$1" == --* ]]; do
    case "$1" in
        --delete) OPTS+=(--delete) ;;
        --fast) OPTS=("${OPTS[@]/--checksum/}") ;;
        --dry-run) OPTS+=(--dry-run); DRY=1 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
NAME="${1:?usage: publish.sh [--delete] [--fast] [--dry-run] <name> [scale]}"
SCALE="${2:-6}"
SRC="$SRC_ROOT/$NAME"
DST="$DST_ROOT/$NAME"
[[ -d "$SRC/x$SCALE" ]] || { echo "no pack at $SRC/x$SCALE" >&2; exit 1; }
[[ "$NAME" != */* && "$NAME" != .* ]] || { echo "bad pack name" >&2; exit 2; }
mkdir -p "$DST"
t0=$(date +%s.%N)
# OPTS may contain an empty element after --fast; filter it out.
CLEAN=()
for o in "${OPTS[@]}"; do [[ -n "$o" ]] && CLEAN+=("$o"); done
rsync "${CLEAN[@]}" "$SRC/" "$DST/" | sed 's/^/  /' | tail -n 20
if [[ $DRY -eq 0 ]]; then
    touch "$SRC/x$SCALE/.reload" "$DST/x$SCALE/.reload"
fi
t1=$(date +%s.%N)
echo "published $SRC -> $DST ($(awk "BEGIN{printf \"%.1f\", $t1 - $t0}") s)"
