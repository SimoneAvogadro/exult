#!/bin/bash
# tests/game/regen_goldens.sh - records golden/buildmap{0,1,2}.sha256 (hash lists only, never
# pixels) with the reference build: upstream 8b6ab6b43 + P3 (+ P11), built from the exult-upstream
# worktree (DESIGN.md sections 6.4 and 7.1). Each level is recorded twice and must give the same
# list both times. The new lists replace the committed ones; commit them with the cause in the
# message (for example "upstream merged P1", or a change of this harness).
# Needs U7_BG_STATIC. Run it from any directory; the lists go into the source tree's tests/game/golden.
# Usage: U7_BG_STATIC=... regen_goldens.sh <reference-build-dir>   (or UPSTREAM_BUILD=...)
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ref_build=${1:-${UPSTREAM_BUILD:-}}
if [ -z "$ref_build" ]; then
	echo "Usage: U7_BG_STATIC=... $0 <reference-build-dir> (the build of upstream + P3)" >&2
	exit 2
fi
if [ -z "${U7_BG_STATIC:-}" ]; then
	echo "ERROR: U7_BG_STATIC is not set" >&2
	exit 2
fi

tmp=${HIRES_TEST_TMP:-$(cd "$ref_build/.." && pwd)/tmp}    # As lib.sh.
mkdir -p "$tmp" || exit 2
work=$(mktemp -d "$tmp/regen-goldens.XXXXXX") || exit 2
mkdir -p "$work/run1" "$work/run2" || exit 2
for run in run1 run2; do
	if ! bash "$here/buildmap_golden.sh" --record "$work/$run" "$ref_build"; then
		echo "ERROR: buildmap with $ref_build failed ($run); nothing changed" >&2
		exit 1
	fi
done
for level in 0 1 2; do
	if ! cmp -s "$work/run1/buildmap$level.sha256" "$work/run2/buildmap$level.sha256"; then
		echo "ERROR: buildmap $level gave two different lists with $ref_build; nothing changed ($work)" >&2
		exit 1
	fi
done
mkdir -p "$here/golden" || exit 2
for level in 0 1 2; do
	if cmp -s "$work/run1/buildmap$level.sha256" "$here/golden/buildmap$level.sha256"; then
		echo "unchanged: golden/buildmap$level.sha256"
	else
		cp "$work/run1/buildmap$level.sha256" "$here/golden/buildmap$level.sha256" || exit 2
		echo "UPDATED: golden/buildmap$level.sha256 (commit it with the cause in the message)"
	fi
done
rm -rf "$work"
