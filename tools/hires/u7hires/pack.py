"""Pack writer (DESIGN §5.1-§5.4, §5.7).

Layout written::

    <root>/pack.txt                          game=BG, scale=6, palette_crc32=<8 hex>, edge, route, title
    <root>/x6/.reload                        touched after every write (dev hot reload)
    <root>/x6/flats/<SSSS>/<SSSS>_<FF>.png   48x48, colour type 3, raw indices, PLTE = full palette 0,
                                             tEXt Exult-Src-CRC32 (CRC32 of the 64-byte 1x flat) and
                                             Exult-Origin (route), before IDAT
    <root>/x6/flats/<SSSS>/<SSSS>_<FF>.json  sidecar (ignored by the engine)
    <root>/x6/flats.bundle                   optional (``--bundle``), format below

Every file is written to ``*.tmp`` and renamed. Output bytes are deterministic (no timestamps).

``flats.bundle`` (little-endian; tooling proposal of §5.7, to be matched by ``hires_bundle.cc``)::

    header (16 bytes): "U7HB" | u16 version=1 | u16 scale | u32 palette_crc32 | u32 count
    count x entry, sorted by (shape, frame):
        u16 shape | u8 frame | u8 flags (bit0: guard present) | u32 guard | (8*scale)^2 indices
    file size must equal 16 + count * (8 + (8*scale)^2)
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np

from .check import GUARD_KEY, ORIGIN_KEY, Provider, check_tile_array, read_pack_txt
from .hashing import crc32, palette_crc32
from .pngio import encode_indexed
from .util import atomic_write_bytes, atomic_write_json, get_logger, key_name, touch

BUNDLE_MAGIC = b"U7HB"
BUNDLE_VERSION = 1
BUNDLE_HDR = struct.Struct("<4sHHII")
BUNDLE_ENTRY = struct.Struct("<HBBI")
DEFAULT_PACKS = "/home/simonea/ultima7_exult/packs"


class BundleError(ValueError):
    pass


def tile_png_bytes(tile: np.ndarray, pal8: np.ndarray, src_flat: np.ndarray | None, origin: str | None) -> bytes:
    texts = []
    if src_flat is not None:
        texts.append((GUARD_KEY, f"{crc32(np.asarray(src_flat, np.uint8).tobytes()):08x}"))
    if origin:
        texts.append((ORIGIN_KEY, origin))
    return encode_indexed(np.asarray(tile, np.uint8), pal8, texts)


def pack_txt_text(game: str, scale: int, pal_crc: int, edge: str, route: str, title: str = "") -> str:
    lines = [f"game={game}", f"scale={scale}", f"palette_crc32={pal_crc:08x}", f"edge={edge}", f"route={route}"]
    if title:
        lines.append(f"title={title}")
    return "\n".join(lines) + "\n"


def palette_crc_from_ref(ref_txt: str) -> int:
    """palette CRC32 as written by the engine's ``--dump-art`` ref.txt (copied, never recomputed)."""
    from .check import parse_crc
    meta = read_pack_txt(ref_txt)
    for k in ("palette_crc32", "palette CRC32", "palette_crc"):
        if k in meta:
            v = parse_crc(meta[k])
            if v is not None:
                return v
    raise ValueError(f"no palette_crc32 in {ref_txt}")


def apply_edge_nn3(tile: np.ndarray, src: np.ndarray, scale: int = 6) -> np.ndarray:
    """Edge contract nn3: the outer S/2 px band equals the NN replication of the 1x border pixels."""
    b = scale // 2
    nn = np.repeat(np.repeat(np.asarray(src), scale, 0), scale, 1)
    out = np.array(tile, np.uint8, copy=True)
    out[:b, :] = nn[:b, :]
    out[-b:, :] = nn[-b:, :]
    out[:, :b] = nn[:, :b]
    out[:, -b:] = nn[:, -b:]
    return out


def write_bundle(path: str, entries: list[tuple[int, int, np.ndarray, int | None]], scale: int, pal_crc: int) -> None:
    """entries: (shape, frame, tile, guard or None)."""
    side = 8 * scale
    parts = [BUNDLE_HDR.pack(BUNDLE_MAGIC, BUNDLE_VERSION, scale, pal_crc, len(entries))]
    for s, f, t, g in sorted(entries, key=lambda e: (e[0], e[1])):
        t = np.asarray(t, np.uint8)
        if t.shape != (side, side):
            raise ValueError("bad tile size for bundle")
        parts.append(BUNDLE_ENTRY.pack(s, f, 1 if g is not None else 0, 0 if g is None else g))
        parts.append(t.tobytes())
    atomic_write_bytes(path, b"".join(parts))


def read_bundle(path: str, expect_scale: int | None = None, expect_palette_crc: int | None = None):
    """Parse a bundle with the B0 checks; returns (entries, header)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < BUNDLE_HDR.size:
        raise BundleError("truncated header")
    magic, ver, scale, pcrc, count = BUNDLE_HDR.unpack_from(data, 0)
    if magic != BUNDLE_MAGIC:
        raise BundleError("bad magic")
    if ver != BUNDLE_VERSION:
        raise BundleError(f"version {ver}")
    if expect_scale is not None and scale != expect_scale:
        raise BundleError(f"scale {scale} != {expect_scale}")
    if expect_palette_crc is not None and pcrc != expect_palette_crc:
        raise BundleError(f"palette_crc32 {pcrc:08x} != {expect_palette_crc:08x}")
    side = 8 * scale
    esz = BUNDLE_ENTRY.size + side * side
    if len(data) != BUNDLE_HDR.size + count * esz:
        raise BundleError(f"size {len(data)} != header + {count} entries")
    entries = []
    for i in range(count):
        off = BUNDLE_HDR.size + i * esz
        s, fr, flags, guard = BUNDLE_ENTRY.unpack_from(data, off)
        tile = np.frombuffer(data, np.uint8, side * side, off + BUNDLE_ENTRY.size).reshape(side, side)
        entries.append({"shape": s, "frame": fr, "guarded": bool(flags & 1), "guard": guard, "tile": tile})
    return entries, {"version": ver, "scale": scale, "palette_crc32": pcrc, "count": count}


class PackWriter:
    """Write tiles into ``<packs>/<name>`` after the engine-equivalent checks."""

    def __init__(self, root: str, provider: Provider, scale: int = 6, game: str = "BG", route: str = "",
                 edge: str = "none", title: str = "", pal_crc: int | None = None, layout: str = "shape-groups",
                 origin: str | None = None):
        if edge not in ("none", "nn3"):
            raise ValueError("edge must be none or nn3")
        self.root, self.provider, self.scale, self.game = root, provider, scale, game
        self.route, self.edge, self.title, self.layout = route, edge, title, layout
        self.origin = origin or route
        self.pal_crc = palette_crc32(provider.pal8) if pal_crc is None else pal_crc
        self.written: list[tuple[int, int]] = []
        self.refused: list[dict] = []
        self.bundle_entries: list = []
        self.xdir = os.path.join(root, f"x{scale}")

    def tile_path(self, shape: int, frame: int) -> str:
        n = key_name(shape, frame)
        if self.layout == "flat":
            return os.path.join(self.xdir, "flats", n + ".png")
        return os.path.join(self.xdir, "flats", f"{shape:04d}", n + ".png")

    def add(self, shape: int, frame: int, tile: np.ndarray, sidecar: dict | None = None, force: bool = False,
            loose: bool = True, bundle: bool = False) -> bool:
        src = self.provider.flat(shape, frame)
        if self.edge == "nn3" and src is not None:
            tile = apply_edge_nn3(tile, src, self.scale)
        findings = check_tile_array(tile, shape, frame, self.provider, self.scale, offline=False, edge=self.edge)
        rejects = [f for f in findings if f.severity == "reject"]
        if rejects and not force:
            self.refused.append({"key": [shape, frame], "rules": [f.rule for f in rejects],
                                 "detail": "; ".join(f.detail for f in rejects)})
            return False
        if loose:
            p = self.tile_path(shape, frame)
            atomic_write_bytes(p, tile_png_bytes(tile, self.provider.pal8, src, self.origin))
            if sidecar is not None:
                sc = {"key": key_name(shape, frame), "shape": shape, "frame": frame,
                      "src_crc": f"{crc32(np.asarray(src, np.uint8).tobytes()):08x}", "route": self.route,
                      "edge": self.edge, **sidecar}
                atomic_write_json(p[:-4] + ".json", sc)
        if bundle:
            self.bundle_entries.append((shape, frame, np.asarray(tile, np.uint8),
                                        crc32(np.asarray(src, np.uint8).tobytes())))
        self.written.append((shape, frame))
        return True

    def finish(self) -> dict:
        os.makedirs(self.xdir, exist_ok=True)
        atomic_write_bytes(os.path.join(self.root, "pack.txt"),
                           pack_txt_text(self.game, self.scale, self.pal_crc, self.edge, self.route, self.title).encode())
        if self.bundle_entries:
            write_bundle(os.path.join(self.xdir, "flats.bundle"), self.bundle_entries, self.scale, self.pal_crc)
        touch(os.path.join(self.xdir, ".reload"))
        summary = {"root": self.root, "written": len(self.written), "refused": self.refused,
                   "palette_crc32": f"{self.pal_crc:08x}", "edge": self.edge, "route": self.route}
        on_disk = {k: v for k, v in summary.items() if k != "root"}      # keep pack bytes path-independent
        atomic_write_json(os.path.join(self.xdir, "flats", "_pack_summary.json"), on_disk)
        return summary


def load_candidates(path: str) -> tuple[dict, dict]:
    """Read a candidate set written by a route: ``tiles.npz`` (+ ``tiles.json`` with per-key stats)."""
    import json
    with np.load(os.path.join(path, "tiles.npz")) as z:
        karr, tarr = z["keys"], z["tiles"]        # read each array once
    keys = [tuple(int(v) for v in k) for k in karr]
    tiles = {k: tarr[i] for i, k in enumerate(keys)}
    meta = {}
    jp = os.path.join(path, "tiles.json")
    if os.path.exists(jp):
        with open(jp) as f:
            meta = json.load(f)
    return tiles, meta


def save_candidates(path: str, tiles: dict, meta: dict) -> None:
    os.makedirs(path, exist_ok=True)
    keys = sorted(tiles)
    tmp = os.path.join(path, "tiles.tmp.npz")
    np.savez_compressed(tmp, keys=np.array(keys, np.int32).reshape(-1, 2),
                        tiles=np.stack([tiles[k] for k in keys]) if keys else np.zeros((0, 48, 48), np.uint8))
    os.replace(tmp, os.path.join(path, "tiles.npz"))
    atomic_write_json(os.path.join(path, "tiles.json"), meta)


def tile_qa(tiles: dict, provider: Provider, scale: int = 6) -> dict:
    """Per-tile QA metrics for the sidecars (qa.tile_metrics: B1-B4, A2, A3)."""
    from .qa import tile_metrics
    from .quant import Quantizer
    from .rules import engine_ramps, ramp_lut
    keys = [k for k in sorted(tiles) if provider.flat(*k) is not None and tiles[k].shape == (8 * scale, 8 * scale)]
    if not keys or provider.pal6 is None:
        return {}
    lut = ramp_lut(engine_ramps(provider.pal6))
    m = tile_metrics(keys, [tiles[k] for k in keys], [provider.flat(*k) for k in keys],
                     Quantizer(provider.pal8, lut), lut, scale)
    names = ("b1", "b2", "b3_mean", "b4", "a2_bad", "a3_p4", "a3_dropped")
    return {k: {n: round(float(m[n][i]), 5) for n in names} for i, k in enumerate(keys)}


def build_pack(candidates: str, name: str, provider: Provider, packs_dir: str = DEFAULT_PACKS, edge: str = "none",
               bundle: bool = False, loose: bool = True, title: str = "", ref_txt: str | None = None,
               force: bool = False, layout: str = "shape-groups", scale: int = 6, log=None) -> dict:
    log = log or get_logger()
    tiles, meta = load_candidates(candidates)
    route = meta.get("route", "unknown")
    pal_crc = palette_crc_from_ref(ref_txt) if ref_txt else None
    root = os.path.join(packs_dir, name)
    w = PackWriter(root, provider, scale, route=route, edge=edge, title=title, pal_crc=pal_crc, layout=layout,
                   origin=meta.get("origin", route))
    params = meta.get("params", {})
    per_key = meta.get("keys", {})
    qa_by_key = tile_qa(tiles, provider, scale)
    for k in sorted(tiles):
        kn = key_name(*k)
        sc = {"params": params, "model_sha256": meta.get("model_sha256"), **per_key.get(kn, {}),
              "qa": qa_by_key.get(k, {})}
        w.add(k[0], k[1], tiles[k], sc, force=force, loose=loose, bundle=bundle)
    s = w.finish()
    log.info("pack %s: %d tiles written, %d refused", root, s["written"], len(s["refused"]))
    return s


def main(argv=None) -> int:
    from .io import World
    ap = argparse.ArgumentParser(prog="mkpack.py", description="Build a pack from a route's candidate set "
                                 "(engine checks first; loose PNG groups and/or flats.bundle).")
    ap.add_argument("candidates", help="candidate dir written by route3.py/route2.py (tiles.npz)")
    ap.add_argument("name", help="pack name: written to <packs>/<name>")
    ap.add_argument("--packs", default=DEFAULT_PACKS)
    ap.add_argument("--static", default=None)
    ap.add_argument("--edge", choices=("none", "nn3"), default="none")
    ap.add_argument("--bundle", action="store_true", help="also write x6/flats.bundle")
    ap.add_argument("--no-loose", action="store_true", help="bundle only")
    ap.add_argument("--layout", choices=("shape-groups", "flat"), default="shape-groups")
    ap.add_argument("--title", default="")
    ap.add_argument("--ref", default=None, help="engine dump ref.txt: copy palette_crc32 from it")
    ap.add_argument("--force", action="store_true", help="write tiles even if they fail engine rules")
    a = ap.parse_args(argv)
    world = World.load(a.static)
    s = build_pack(a.candidates, a.name, Provider.from_world(world), a.packs, a.edge, a.bundle, not a.no_loose,
                   a.title, a.ref, a.force, a.layout)
    for r in s["refused"][:20]:
        print("refused", r)
    return 0 if not s["refused"] else 1


if __name__ == "__main__":
    sys.exit(main())
