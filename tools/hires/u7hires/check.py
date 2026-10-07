"""Pack validator mirroring the engine's load-time rules (DESIGN §5.5), with the same rule IDs.

Engine rules (severity as in the engine):

==== ======================================================================= ==============
ID   Rule                                                                    Severity
==== ======================================================================= ==============
N1   name ``^(\\d{4})_(\\d{2})\\.png$``; frame < 32; 0 <= shape < num_shapes;     reject
     the provider returns a flat for (shape, frame)
F1   colour type 3 (palette)                                                 reject
F2   every used index i: i < len(PLTE), then PLTE[i] == pal0_8bit[i]          reject (first bad index)
F3   exact size (8S x 8S), checked on IHDR before decoding                    reject
F4   tRNS present                                                            warning
P0   index 0xFF in a flat                                                    reject
P4   cycling pixel outside its in-tile clamped parent neighbourhood           reject if > 0.5 %, else warning
G1   tEXt Exult-Src-CRC32 present and != CRC32 of the live 1x flat           reject
G2   any reject inside a group directory -> whole group skipped              group skipped
R1   pack.txt palette_crc32 != CRC32 of Pal8.rgb                             root disabled
B0   flats.bundle header/size invalid (see pack.py for the format)           bundle disabled
==== ======================================================================= ==============

Offline rules (``severity = "offline"``; failures only with ``--strict``): P2 (6x6 block ramp ==
parent ramp, engine ``Palette::get_ramps``) and E1 (the edge contract declared in pack.txt).

Rule order per file (the first reject is the file's primary rule): N1, F3, F1, F2, F4, P0, P4, G1.
Layout (§5.1): ``<root>/pack.txt``, ``<root>/x<S>/flats/*.png`` (single tiles),
``<root>/x<S>/flats/<group>/*.png`` (strict groups; ``*.off`` disabled; ``_*`` and ``.*`` ignored),
``<root>/x<S>/flats.bundle``; non-PNG files are ignored.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import asdict, dataclass, field

import numpy as np

from .hashing import crc32, palette_crc32
from .pngio import PngError, decode_indexed, parse_png, set_texts
from .rules import BORDER_INDEX, nn_upscale, p4_violations, p4_verdict, ramp_lut as make_ramp_lut

NAME_RE = re.compile(r"^(\d{4})_(\d{2})\.png$")
TERRAIN_RE = re.compile(r"^[0-9a-f]{16}\.png$")
GUARD_KEY = "Exult-Src-CRC32"
ORIGIN_KEY = "Exult-Origin"
TERRAIN_KEY = "Exult-Terrain-Key"
HEX8_RE = re.compile(r"^(0x)?[0-9a-fA-F]{8}$")
RULE_ORDER = ["N1", "F3", "F1", "F2", "F4", "P0", "P4", "G1"]


@dataclass
class Finding:
    rule: str
    severity: str           # reject | warning | offline | info | group-skipped | root-disabled | bundle-disabled
    path: str
    detail: str = ""
    key: list | None = None


@dataclass
class TileResult:
    path: str
    key: tuple[int, int] | None
    findings: list[Finding] = field(default_factory=list)
    guarded: bool = False
    tile: np.ndarray | None = None

    @property
    def rejected(self) -> bool:
        return any(f.severity == "reject" for f in self.findings)

    @property
    def primary(self) -> str | None:
        rej = [f.rule for f in self.findings if f.severity == "reject"]
        if not rej:
            return None
        return sorted(rej, key=lambda r: RULE_ORDER.index(r) if r in RULE_ORDER else 99)[0]


class Provider:
    """What the engine glue provides: Pal8, shape count, and the live 1x flats."""

    def __init__(self, pal8: np.ndarray, num_shapes: int, flat_fn, pal6: np.ndarray | None = None):
        self.pal8 = np.asarray(pal8, np.uint8).reshape(256, 3)
        self.num_shapes = num_shapes
        self._flat = flat_fn
        self.pal6 = pal6
        self._ramp = None

    @classmethod
    def from_world(cls, world) -> "Provider":
        return cls(world.pal8, world.shapes.num_shapes, world.shapes.flat, world.pal6)

    @classmethod
    def from_dict(cls, pal8, flats: dict, num_shapes: int, pal6=None) -> "Provider":
        return cls(pal8, num_shapes, lambda s, f: flats.get((s, f & 31)), pal6)

    def flat(self, shape: int, frame: int) -> np.ndarray | None:
        if shape < 0 or shape >= self.num_shapes:
            return None
        return self._flat(shape, frame)

    @property
    def ramps(self) -> np.ndarray | None:
        if self._ramp is None and self.pal6 is not None:
            from .rules import engine_ramps
            self._ramp = make_ramp_lut(engine_ramps(self.pal6))
        return self._ramp


# ---------------------------------------------------------------------------- per tile
def check_name(name: str, provider: Provider) -> tuple[tuple[int, int] | None, Finding | None]:
    m = NAME_RE.match(name)
    if not m:
        return None, Finding("N1", "reject", name, "name does not match SSSS_FF.png")
    shape, frame = int(m.group(1)), int(m.group(2))
    if frame >= 32:
        return (shape, frame), Finding("N1", "reject", name, f"frame {frame} >= 32")
    if shape >= provider.num_shapes:
        return (shape, frame), Finding("N1", "reject", name, f"shape {shape} >= num_shapes {provider.num_shapes}")
    if provider.flat(shape, frame) is None:
        return (shape, frame), Finding("N1", "reject", name, f"({shape},{frame}) is not a flat (RLE or missing)")
    return (shape, frame), None


def check_pixels(tile: np.ndarray, src: np.ndarray, scale: int, path: str, key, provider: Provider | None = None,
                 offline: bool = True, edge: str = "none") -> list[Finding]:
    """P0, P4 (+ offline P2, E1) on decoded indices against the live 1x flat."""
    out = []
    k = list(key) if key else None
    nff = int((tile == BORDER_INDEX).sum())
    if nff:
        out.append(Finding("P0", "reject", path, f"{nff} pixels with index 0xFF", k))
    bad = p4_violations(tile, src, scale)
    v = p4_verdict(bad, tile.size)
    if v != "ok":
        out.append(Finding("P4", v, path, f"{bad} non-compliant cycling pixels of {tile.size}", k))
    if offline and provider is not None and provider.ramps is not None:
        lut = provider.ramps
        h, w = src.shape
        rb = lut[tile].reshape(h, scale, w, scale).transpose(0, 2, 1, 3).reshape(h, w, scale * scale)
        nr = int(lut.max()) + 2
        cnt = np.zeros((h, w, nr), np.int32)
        for i in range(scale * scale):
            np.add.at(cnt, (np.arange(h)[:, None], np.arange(w)[None, :], rb[..., i] + 1), 1)
        dom = cnt.argmax(-1) - 1
        moved = int((dom != lut[src]).sum())
        if moved:
            out.append(Finding("P2", "offline", path, f"{moved} of {h * w} blocks change ramp", k))
    if offline and edge == "nn3":
        b = scale // 2
        nn = nn_upscale(src, scale)
        band = np.zeros(tile.shape, bool)
        band[:b, :] = band[-b:, :] = band[:, :b] = band[:, -b:] = True
        nbad = int((tile[band] != nn[band]).sum())
        if nbad:
            out.append(Finding("E1", "offline", path, f"{nbad} band pixels differ from NN of the 1x border", k))
    return out


def check_png_bytes(data: bytes, name: str, provider: Provider, scale: int = 6, path: str | None = None,
                    offline: bool = True, edge: str = "none") -> TileResult:
    """All per-file rules for one flat override PNG (``name`` is the file name)."""
    path = path or name
    res = TileResult(path, None)
    key, f = check_name(name, provider)
    res.key = key
    if f:
        f.path = path
        res.findings.append(f)
        return res
    side = 8 * scale
    try:
        info = parse_png(data)
    except PngError as e:
        res.findings.append(Finding("F1", "reject", path, f"unreadable PNG: {e}", list(key)))
        return res
    if (info.width, info.height) != (side, side):
        res.findings.append(Finding("F3", "reject", path, f"size {info.width}x{info.height}, expected {side}x{side}",
                                    list(key)))
        return res
    if info.color_type != 3:
        res.findings.append(Finding("F1", "reject", path, f"colour type {info.color_type}: quantize to palette 0 first "
                                    "(tools/hires/quantize)", list(key)))
        return res
    if info.plte is None:
        res.findings.append(Finding("F1", "reject", path, "palette image without PLTE", list(key)))
        return res
    try:
        tile = decode_indexed(data, info)
    except PngError as e:
        res.findings.append(Finding("F1", "reject", path, f"undecodable image data: {e}", list(key)))
        return res
    used = np.nonzero(np.bincount(tile.ravel(), minlength=256))[0]
    n = len(info.plte)
    for i in used:
        if i >= n:
            res.findings.append(Finding("F2", "reject", path, f"index {i} >= PLTE size {n}", list(key)))
            break
        if not np.array_equal(info.plte[i], provider.pal8[i]):
            res.findings.append(Finding("F2", "reject", path, f"PLTE[{i}] = {tuple(int(v) for v in info.plte[i])} != "
                                        f"pal0[{i}] = {tuple(int(v) for v in provider.pal8[i])} (load palette/pal0.gpl, "
                                        "keep indices)", list(key)))
            break
    if info.trns is not None:
        res.findings.append(Finding("F4", "warning", path, "tRNS present (ignored, indices are read raw)", list(key)))
    src = provider.flat(*key)
    res.findings.extend(check_pixels(tile, src, scale, path, key, provider, offline, edge))
    g = info.texts.get(GUARD_KEY)
    if g is not None:
        res.guarded = True
        live = crc32(np.asarray(src, np.uint8).tobytes())
        if not HEX8_RE.match(g.strip()):
            res.findings.append(Finding("G1", "reject", path, f"malformed {GUARD_KEY} '{g}'", list(key)))
        elif int(g.strip(), 16) != live:
            res.findings.append(Finding("G1", "reject", path, f"stale: built against different shapes.vga "
                                        f"(guard {g.strip().lower()}, live {live:08x})", list(key)))
    res.tile = tile
    return res


def check_tile_array(tile: np.ndarray, shape: int, frame: int, provider: Provider, scale: int = 6,
                     offline: bool = True, edge: str = "none") -> list[Finding]:
    """The rules that can fail for an in-memory tile written by our own PNG writer (N1, F3, P0, P4, P2, E1)."""
    name = f"{shape:04d}_{frame:02d}.png"
    key, f = check_name(name, provider)
    if f:
        return [f]
    side = 8 * scale
    if tile.shape != (side, side):
        return [Finding("F3", "reject", name, f"size {tile.shape}, expected {side}x{side}", [shape, frame])]
    return check_pixels(np.asarray(tile, np.uint8), provider.flat(shape, frame), scale, name, key, provider,
                        offline, edge)


# ---------------------------------------------------------------------------- pack.txt / root
def read_pack_txt(path: str) -> dict[str, str]:
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def parse_crc(v: str) -> int | None:
    v = v.strip().lower()
    if v.startswith("0x"):
        v = v[2:]
    try:
        return int(v, 16) & 0xFFFFFFFF
    except ValueError:
        return None


@dataclass
class Report:
    root: str
    scale: int
    tiles: int = 0
    loaded: int = 0
    rejected: int = 0
    groups_skipped: int = 0
    unguarded: int = 0
    warnings: int = 0
    offline: int = 0
    root_disabled: bool = False
    findings: list[Finding] = field(default_factory=list)
    loaded_keys: list = field(default_factory=list)

    def ok(self, strict: bool = False) -> bool:
        if self.root_disabled or self.rejected or self.groups_skipped:
            return False
        if any(f.severity == "bundle-disabled" for f in self.findings):
            return False
        return not (strict and (self.offline or self.warnings))

    def summary(self) -> dict:
        d = {k: getattr(self, k) for k in ("root", "scale", "tiles", "loaded", "rejected", "groups_skipped",
                                            "unguarded", "warnings", "offline", "root_disabled")}
        rules: dict[str, int] = {}
        for f in self.findings:
            rules[f"{f.rule}:{f.severity}"] = rules.get(f"{f.rule}:{f.severity}", 0) + 1
        d["by_rule"] = dict(sorted(rules.items()))
        return d


def _list_pngs(d: str) -> list[str]:
    return sorted(e for e in os.listdir(d) if os.path.isfile(os.path.join(d, e)) and e.lower().endswith(".png"))


def check_root(root: str, provider: Provider, scale: int = 6, offline: bool = True, restamp: bool = False,
               keep_tiles: bool = False) -> Report:
    """Validate one pack root the way the engine loads it (§3.5, §5.5); returns a Report."""
    rep = Report(root, scale)
    edge = "none"
    ptxt = os.path.join(root, "pack.txt")
    if os.path.exists(ptxt):
        meta = read_pack_txt(ptxt)
        edge = meta.get("edge", "none")
        if "palette_crc32" in meta:
            want = parse_crc(meta["palette_crc32"])
            live = palette_crc32(provider.pal8)
            if want != live:
                rep.root_disabled = True
                rep.findings.append(Finding("R1", "root-disabled", ptxt, f"palette_crc32 {meta['palette_crc32']} != "
                                            f"palette 0 CRC {live:08x} (wrong game?)"))
                return rep
        if meta.get("scale") and meta["scale"] != str(scale):
            rep.findings.append(Finding("R1", "info", ptxt, f"pack.txt scale={meta['scale']} (checking x{scale})"))
    else:
        rep.findings.append(Finding("R1", "info", ptxt, "no pack.txt: loaded file by file"))
    xdir = os.path.join(root, f"x{scale}")
    bundle_keys: set = set()
    bpath = os.path.join(xdir, "flats.bundle")
    if os.path.exists(bpath):
        from .pack import read_bundle, BundleError
        try:
            entries, hdr = read_bundle(bpath, expect_scale=scale, expect_palette_crc=palette_crc32(provider.pal8))
            for e in entries:
                k = (e["shape"], e["frame"])
                rep.tiles += 1
                findings = []
                key, f = check_name(f"{k[0]:04d}_{k[1]:02d}.png", provider)
                if f:
                    f.path = f"{bpath}#{k[0]:04d}_{k[1]:02d}"
                    findings.append(f)
                else:
                    src = provider.flat(*k)
                    findings = check_pixels(e["tile"], src, scale, f"{bpath}#{k[0]:04d}_{k[1]:02d}", k, provider,
                                            offline, edge)
                    if e["guarded"] and e["guard"] != crc32(np.asarray(src, np.uint8).tobytes()):
                        findings.append(Finding("G1", "reject", f"{bpath}#{k[0]:04d}_{k[1]:02d}", "stale guard", list(k)))
                rep.findings.extend(findings)
                if any(x.severity == "reject" for x in findings):
                    rep.rejected += 1
                else:
                    rep.loaded += 1
                    bundle_keys.add(k)
                    rep.unguarded += 0 if e["guarded"] else 1
        except BundleError as ex:
            rep.findings.append(Finding("B0", "bundle-disabled", bpath, str(ex)))
    fdir = os.path.join(xdir, "flats")
    loaded: dict[tuple[int, int], str] = {}
    if os.path.isdir(fdir):
        units: list[tuple[str | None, list[str]]] = [(None, [os.path.join(fdir, n) for n in _list_pngs(fdir)])]
        for e in sorted(os.listdir(fdir)):
            p = os.path.join(fdir, e)
            if not os.path.isdir(p):
                continue
            if e.startswith("_") or e.startswith("."):
                continue
            if e.endswith(".off"):
                rep.findings.append(Finding("G2", "info", p, "disabled group (.off)"))
                continue
            units.append((e, [os.path.join(p, n) for n in _list_pngs(p)]))
        for group, files in units:
            results = []
            for fp in files:
                with open(fp, "rb") as fh:
                    data = fh.read()
                r = check_png_bytes(data, os.path.basename(fp), provider, scale, fp, offline, edge)
                results.append((fp, data, r))
            rep.tiles += len(results)
            group_bad = group is not None and any(r.rejected for _, _, r in results)
            for fp, data, r in results:
                rep.findings.extend(r.findings)
                if r.rejected or group_bad:
                    rep.rejected += 1
                    continue
                if r.key in loaded:
                    rep.findings.append(Finding("N1", "warning", fp, f"duplicate key, also {loaded[r.key]}", list(r.key)))
                loaded[r.key] = fp
                rep.loaded_keys.append(list(r.key))
                if not r.guarded:
                    rep.unguarded += 1
                    if restamp:
                        from .util import atomic_write_bytes
                        src = provider.flat(*r.key)
                        atomic_write_bytes(fp, set_texts(data, {GUARD_KEY: f"{crc32(np.asarray(src, np.uint8).tobytes()):08x}"}))
                        rep.findings.append(Finding("G1", "info", fp, "restamped guard", list(r.key)))
            if group_bad:
                rep.groups_skipped += 1
                first = next(r for _, _, r in results if r.rejected)
                rep.findings.append(Finding("G2", "group-skipped", os.path.join(fdir, group),
                                            f"group skipped: {first.path} failed {first.primary}"))
    rep.loaded = len(loaded) + len(bundle_keys - set(loaded))
    rep.warnings = sum(1 for f in rep.findings if f.severity == "warning")
    rep.offline = sum(1 for f in rep.findings if f.severity == "offline")
    return rep


def main(argv=None) -> int:
    from .io import World
    from .util import json_dumps
    ap = argparse.ArgumentParser(prog="hirescheck.py", description="Validate a hi-res pack like the engine does "
                                 "(rules N1 F1 F2 F3 F4 P0 P4 G1 G2 R1 B0, offline P2 E1).")
    ap.add_argument("root", nargs="+", help="pack root(s) (directory holding pack.txt and x6/)")
    ap.add_argument("--static", default=None, help="BG STATIC dir (provider for flats and palette 0)")
    ap.add_argument("--scale", type=int, default=6)
    ap.add_argument("--no-offline", action="store_true", help="skip the offline rules P2/E1")
    ap.add_argument("--strict", action="store_true", help="fail on warnings and offline rules too")
    ap.add_argument("--restamp", action="store_true", help="add Exult-Src-CRC32 to unguarded loaded tiles")
    ap.add_argument("--json", default=None, help="write the full report as JSON")
    ap.add_argument("--max-print", type=int, default=40)
    a = ap.parse_args(argv)
    world = World.load(a.static)
    prov = Provider.from_world(world)
    ok = True
    reports = []
    for root in a.root:
        rep = check_root(root, prov, a.scale, not a.no_offline, a.restamp)
        reports.append(rep)
        s = rep.summary()
        print(f"{root}: tiles {s['tiles']} loaded {s['loaded']} rejected {s['rejected']} groups_skipped "
              f"{s['groups_skipped']} unguarded {s['unguarded']} warnings {s['warnings']} offline {s['offline']}"
              f"{' ROOT DISABLED' if s['root_disabled'] else ''}")
        for k, v in s["by_rule"].items():
            print(f"  {k}: {v}")
        shown = 0
        for f in rep.findings:
            if f.severity in ("info",):
                continue
            if shown >= a.max_print:
                print("  ...")
                break
            print(f"  [{f.rule} {f.severity}] {f.path}: {f.detail}")
            shown += 1
        ok &= rep.ok(a.strict)
    if a.json:
        with open(a.json, "w") as fh:
            fh.write(json_dumps([{"summary": r.summary(), "findings": [asdict(f) for f in r.findings]}
                                 for r in reports]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
