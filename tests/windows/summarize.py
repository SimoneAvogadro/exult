#!/usr/bin/env python3
"""Summarize a run_matrix.sh output directory: one tab-separated row per case.

usage: summarize.py <out dir>     (for example: summarize.py out | column -t -s $'\\t')

Columns: the case, win_rt.sh's exit code, the renderer and the world texture
format the digest names (<tag>_present_renderer, _present_format, and the
read-back passes' present_<fmt>_actual; for runs older than those keys, the
renderer and format of the log's "[hires] world texture" lines), the present
filter of the log, the largest read-back error, the ARGB/INDEX8 difference, the
S = 6 timings (p95 unless named) and the store load time of the log
("[hires] x6: ..., N ms").
Python 3.8 or newer, standard library only.
"""
import glob
import json
import os
import re
import sys

KEYS = [
    ("paint", "bench_s6_paint_p95_ms"),
    ("upload", "bench_s6_upload_p95_ms"),
    ("present", "bench_s6_present_p95_ms"),
    ("walk_frame_med", "bench_s6_walk_frame_median_ms"),
    ("walk_frame_p95", "bench_s6_walk_frame_p95_ms"),
    ("walk_paint_p95", "bench_s6_walk_paint_p95_ms"),
    ("walk_renders", "bench_s6_walk_cache_renders"),
    ("cold_p95", "bench_s6_cold_paint_p95_ms"),
    ("rflats_p95", "bench_s6_render_flats_p95_ms"),
    ("first_s6_ms", "time_s6_ms"),
]


def read(path):
    try:
        with open(path, encoding="latin-1") as f:
            return f.read()
    except OSError:
        return ""


def values(digest, suffix):
    return ",".join(sorted({str(v) for k, v in digest.items() if k.endswith(suffix)})) or "-"


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    out_dir = argv[1]
    print("\t".join(["case", "rc", "renderer", "format", "actual", "filter", "maxerr", "fmtdiff"]
                    + [k for k, _ in KEYS] + ["load"]))
    for path in sorted(glob.glob(os.path.join(out_dir, "*.json"))):
        case = os.path.basename(path)[:-5]
        text = read(path)
        try:
            digest = json.loads(text) if text.strip() else {}
        except ValueError:
            digest = {}
        log = read(os.path.join(out_dir, case + ".log"))
        rc = re.search(r"rc=(\d+)", read(os.path.join(out_dir, case + ".out")))
        filters = ",".join(sorted(set(re.findall(r"present filter (\S+)", log)))) or "-"
        errors = [int(v) for k, v in digest.items() if k.endswith("_max_error")]
        load = re.search(r"\[hires\] x6: .*?, ([0-9.]+) ms", log)
        renderer = values(digest, "_present_renderer")
        if renderer == "-":
            renderer = ",".join(sorted(set(re.findall(r"world texture \S+ \S+ on renderer '([^']*)'", log)))) or "-"
        fmt = values(digest, "_present_format")
        if fmt == "-":
            found = set(re.findall(r"world texture \S+ (INDEX8|ARGB8888) on renderer", log))
            fmt = ",".join(sorted("index8" if f == "INDEX8" else "argb" for f in found)) or "-"
        print("\t".join([case, rc.group(1) if rc else "-", renderer, fmt, values(digest, "_actual"), filters,
                         str(max(errors)) if errors else "-",
                         str(digest.get("present_formats_max_difference", "-"))]
                        + [str(digest.get(k, "-")) for _, k in KEYS] + [load.group(1) if load else "-"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
