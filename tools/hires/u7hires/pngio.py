"""Minimal PNG reader/writer with exact control over raw indices, PLTE, tRNS and tEXt (DESIGN §5.3).

* The writer emits 8-bit colour type 3, the full 256-entry PLTE, optional tEXt chunks before IDAT,
  filter type 0 and zlib level 9, so output bytes are deterministic.
* The reader parses chunks itself (IHDR, PLTE, tRNS, tEXt/zTXt/iTXt before and after IDAT), checks
  CRCs, and decodes colour type 3 at depths 1/2/4/8 to **raw indices** (no rotation, no palette
  lookup). Interlaced images are decoded with Pillow (raw indices too).
"""

from __future__ import annotations

import io as _io
import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

PNG_SIG = b"\x89PNG\r\n\x1a\n"


class PngError(ValueError):
    pass


def chunk(ctype: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)


def text_chunk(key: str, value: str) -> bytes:
    k = key.encode("latin-1")
    if not (1 <= len(k) <= 79) or b"\0" in k:
        raise ValueError("bad tEXt keyword")
    return chunk(b"tEXt", k + b"\0" + value.encode("latin-1"))


def _pack_rows(idx: np.ndarray, bit_depth: int) -> bytes:
    h, w = idx.shape
    if bit_depth == 8:
        rows = idx.astype(np.uint8)
    else:
        per = 8 // bit_depth
        wb = (w + per - 1) // per
        pad = np.zeros((h, wb * per), np.uint8)
        pad[:, :w] = idx
        g = pad.reshape(h, wb, per).astype(np.uint16)
        rows = np.zeros((h, wb), np.uint16)
        for i in range(per):
            rows |= g[:, :, i] << (8 - bit_depth * (i + 1))
        rows = rows.astype(np.uint8)
    raw = np.concatenate([np.zeros((h, 1), np.uint8), rows], 1)
    return raw.tobytes()


def build_png(width: int, height: int, bit_depth: int, color_type: int, raw: bytes,
              plte: bytes | None = None, before_idat: list[bytes] = (), after_idat: list[bytes] = (),
              interlace: int = 0) -> bytes:
    """Assemble a PNG from already-filtered scanline bytes (used to craft test fixtures)."""
    out = [PNG_SIG, chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, interlace))]
    if plte is not None:
        out.append(chunk(b"PLTE", plte))
    out.extend(before_idat)
    out.append(chunk(b"IDAT", zlib.compress(raw, 9)))
    out.extend(after_idat)
    out.append(chunk(b"IEND", b""))
    return b"".join(out)


def encode_indexed(idx: np.ndarray, palette: np.ndarray, texts: list[tuple[str, str]] | dict = (),
                   bit_depth: int = 8, trns: bytes | None = None,
                   texts_after_idat: list[tuple[str, str]] = ()) -> bytes:
    """Encode a palette PNG (colour type 3) with raw indices ``idx`` and PLTE = ``palette``."""
    idx = np.asarray(idx)
    if idx.ndim != 2:
        raise ValueError("idx must be 2-D")
    if idx.min(initial=0) < 0 or idx.max(initial=0) >= (1 << bit_depth):
        raise ValueError("index out of range for bit depth")
    pal = np.asarray(palette, np.uint8).reshape(-1, 3)
    if not (1 <= len(pal) <= 256):
        raise ValueError("PLTE must have 1-256 entries")
    items = list(texts.items()) if isinstance(texts, dict) else list(texts)
    before = [text_chunk(k, v) for k, v in items]
    if trns is not None:
        before.insert(0, chunk(b"tRNS", trns))
    after = [text_chunk(k, v) for k, v in texts_after_idat]
    h, w = idx.shape
    return build_png(w, h, bit_depth, 3, _pack_rows(idx, bit_depth), pal.tobytes(), before, after)


@dataclass
class PngInfo:
    width: int = 0
    height: int = 0
    bit_depth: int = 0
    color_type: int = -1
    interlace: int = 0
    plte: np.ndarray | None = None          # (n, 3) uint8
    trns: bytes | None = None
    texts: dict = field(default_factory=dict)
    chunk_types: list = field(default_factory=list)
    idat: bytes = b""


def parse_png(data: bytes) -> PngInfo:
    """Parse chunks; raise PngError on a bad signature, truncation or a critical-chunk CRC error.
    Ancillary chunks with a bad CRC are dropped (libpng's default)."""
    if data[:8] != PNG_SIG:
        raise PngError("not a PNG (bad signature)")
    info = PngInfo()
    p = 8
    idat = []
    seen_ihdr = False
    while True:
        if p + 8 > len(data):
            raise PngError("truncated PNG (no IEND)")
        n, ctype = struct.unpack_from(">I4s", data, p)
        body = data[p + 8:p + 8 + n]
        if len(body) != n or p + 12 + n > len(data):
            raise PngError("truncated chunk")
        crc = struct.unpack_from(">I", data, p + 8 + n)[0]
        p += 12 + n
        critical = not (ctype[0] & 0x20)
        if (zlib.crc32(ctype + body) & 0xFFFFFFFF) != crc:
            if critical:
                raise PngError(f"CRC error in {ctype.decode('latin-1')}")
            continue
        info.chunk_types.append(ctype.decode("latin-1"))
        if ctype == b"IHDR":
            if n != 13:
                raise PngError("bad IHDR")
            (info.width, info.height, info.bit_depth, info.color_type, _c, _f,
             info.interlace) = struct.unpack(">IIBBBBB", body)
            seen_ihdr = True
        elif not seen_ihdr:
            raise PngError("IHDR is not the first chunk")
        elif ctype == b"PLTE":
            if n % 3 or n == 0 or n > 768:
                raise PngError("bad PLTE length")
            info.plte = np.frombuffer(body, np.uint8).reshape(-1, 3).copy()
        elif ctype == b"tRNS":
            info.trns = bytes(body)
        elif ctype == b"tEXt":
            k, _, v = body.partition(b"\0")
            info.texts.setdefault(k.decode("latin-1"), v.decode("latin-1"))
        elif ctype == b"zTXt":
            k, _, rest = body.partition(b"\0")
            try:
                info.texts.setdefault(k.decode("latin-1"), zlib.decompress(rest[1:]).decode("latin-1"))
            except zlib.error:
                pass
        elif ctype == b"iTXt":
            k, _, rest = body.partition(b"\0")
            try:
                cflag, _cm = rest[0], rest[1]
                rest = rest[2:]
                _lang, _, rest = rest.partition(b"\0")
                _tk, _, txt = rest.partition(b"\0")
                txt = zlib.decompress(txt) if cflag else txt
                info.texts.setdefault(k.decode("latin-1"), txt.decode("utf-8"))
            except (IndexError, zlib.error, UnicodeDecodeError):
                pass
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    info.idat = b"".join(idat)
    return info


def _unfilter(raw: bytes, height: int, stride: int, bpp: int) -> np.ndarray:
    buf = np.frombuffer(raw, np.uint8)
    if len(buf) < height * (stride + 1):
        raise PngError("image data too short")
    out = np.zeros((height, stride), np.uint8)
    prior = np.zeros(stride, np.int32)
    for y in range(height):
        ft = int(buf[y * (stride + 1)])
        line = buf[y * (stride + 1) + 1:(y + 1) * (stride + 1)].astype(np.int32)
        if ft == 0:
            cur = line
        elif ft == 1:
            cur = line.copy()
            for x in range(bpp, stride):
                cur[x] = (cur[x] + cur[x - bpp]) & 255
        elif ft == 2:
            cur = (line + prior) & 255
        elif ft == 3:
            cur = line.copy()
            for x in range(stride):
                left = cur[x - bpp] if x >= bpp else 0
                cur[x] = (cur[x] + ((left + prior[x]) >> 1)) & 255
        elif ft == 4:
            cur = line.copy()
            for x in range(stride):
                a = cur[x - bpp] if x >= bpp else 0
                b = prior[x]
                c = prior[x - bpp] if x >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[x] = (cur[x] + pr) & 255
        else:
            raise PngError(f"bad filter type {ft}")
        out[y] = cur
        prior = cur
    return out


def decode_indexed(data: bytes, info: PngInfo | None = None) -> np.ndarray:
    """Raw palette indices (H, W) uint8 of a colour type 3 PNG (depths 1/2/4/8 expanded)."""
    info = info or parse_png(data)
    if info.color_type != 3:
        raise PngError("not a palette image")
    if info.bit_depth not in (1, 2, 4, 8):
        raise PngError("bad bit depth")
    w, h, bd = info.width, info.height, info.bit_depth
    if info.interlace:
        from PIL import Image
        im = Image.open(_io.BytesIO(data))
        im.load()
        a = np.asarray(im)
        if a.shape != (h, w):
            raise PngError("interlaced decode mismatch")
        return a.astype(np.uint8)
    try:
        raw = zlib.decompress(info.idat)
    except zlib.error as e:
        raise PngError(f"zlib error: {e}") from None
    stride = (w * bd + 7) // 8
    rows = _unfilter(raw, h, stride, 1)
    if bd == 8:
        return rows[:, :w].copy()
    per = 8 // bd
    shifts = np.array([8 - bd * (i + 1) for i in range(per)], np.uint8)
    vals = (rows[:, :, None] >> shifts[None, None, :]) & ((1 << bd) - 1)
    return vals.reshape(h, stride * per)[:, :w].astype(np.uint8)


def read_indexed(path: str) -> tuple[np.ndarray, PngInfo]:
    with open(path, "rb") as f:
        data = f.read()
    info = parse_png(data)
    return decode_indexed(data, info), info


def set_texts(data: bytes, texts: dict[str, str]) -> bytes:
    """Return the PNG with tEXt entries for ``texts`` (replacing same-key tEXt/zTXt/iTXt chunks),
    inserted before the first IDAT. Pixel data is untouched (used by ``hirescheck --restamp``)."""
    if data[:8] != PNG_SIG:
        raise PngError("not a PNG")
    keys = {k.encode("latin-1") for k in texts}
    out = [PNG_SIG]
    p = 8
    inserted = False
    while p < len(data):
        n, ctype = struct.unpack_from(">I4s", data, p)
        whole = data[p:p + 12 + n]
        body = data[p + 8:p + 8 + n]
        p += 12 + n
        if ctype in (b"tEXt", b"zTXt", b"iTXt") and body.partition(b"\0")[0] in keys:
            continue
        if ctype == b"IDAT" and not inserted:
            out.extend(text_chunk(k, v) for k, v in texts.items())
            inserted = True
        out.append(whole)
        if ctype == b"IEND":
            break
    return b"".join(out)
