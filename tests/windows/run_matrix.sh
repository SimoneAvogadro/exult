#!/bin/bash
# tests/windows/run_matrix.sh - the WP-15 renderer matrix (docs-hires/test/windows_matrix.md) of a
# Windows Exult.exe through win_rt.sh: 5 renderers x 2 profiles (320: a 320x200 view at 1:1; 860:
# an 860x300 game area in an 860x360 full area, 3440x1440 window, r = 0.667), at S = 6:
#   <r>-<p>-{argb,index8}-auto  present=1, read-back oracle, bench=300, walk=300
#   <r>-<p>-index8-pixelart     filter=pixelart, read-back oracle, bench=100
#   <r>-<p>-both-auto           format=both: ARGB and INDEX8 read-backs in one process
#   <r>-<p>-art-index8          mode=plain, the pack's art through the real present path,
#                               bench=300, walk=300 (only with a pack)
# Usage: EXULT_WIN_DIR=... U7_BG_STATIC_WIN=... run_matrix.sh <out dir> [pack (Windows path)]
#   RENDERERS overrides the renderer list. Writes <out>/<case>.{out,log,json}; summarize.py <out>
#   makes the table. Exits 1 when a case did not exit 0.
set -u
if [ $# -lt 1 ]; then
	echo "usage: EXULT_WIN_DIR=... U7_BG_STATIC_WIN=... $0 <out dir> [pack]" >&2
	exit 2
fi
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
M=$1
pack=${2:-}
mkdir -p "$M" || exit 2
L=$(wslpath -u "${EXULT_WIN_DIR%\\}") || exit 2
P320="tx=800,ty=1330,w=320,h=200,scales=6"
P860="tx=780,ty=1320,w=860,h=360,game=860x300,scales=6,window=3440x1440"
failed=0
run() { # run <case> <renderer> <spec> [pack]
	local c=$1 r=$2 spec=$3 hires=${4:-}
	echo "== $c ($r)"
	SDL_RENDER_DRIVER=$r "$here/win_rt.sh" "m-$c" "$spec" ${hires:+"$hires"} > "$M/$c.out" 2>&1 || failed=1
	head -n 3 "$M/$c.out"
	local sb=$L/rt/m-$c
	cp "$sb/run.log" "$M/$c.log"
	cp "$sb/out/digest.json" "$M/$c.json" 2> /dev/null || echo '{}' > "$M/$c.json"
	rm -rf "$sb"
}
for r in ${RENDERERS:-software direct3d11 direct3d12 vulkan opengl}; do
	for prof in 320 860; do
		ps=P$prof
		base=${!ps}
		for fmt in argb index8; do
			run "$r-$prof-$fmt-auto" "$r" "$base,present=1,format=$fmt,bench=300,walk=300"
		done
		run "$r-$prof-index8-pixelart" "$r" "$base,present=1,format=index8,filter=pixelart,bench=100"
		run "$r-$prof-both-auto" "$r" "$base,present=1,format=both"
		if [ -n "$pack" ]; then
			run "$r-$prof-art-index8" "$r" "$base,mode=plain,overrides=yes,present=1,format=index8,bench=300,walk=300" "$pack"
		fi
	done
done
[ $failed -eq 0 ]
