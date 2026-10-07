"""Hashes shared with the engine (DESIGN §5.2, §5.4): CRC32 (IEEE), FNV-1a-64, T1 terrain key."""

from __future__ import annotations

import zlib

import numpy as np

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
T1_MAGIC = b"U7TK"
T1_VERSION = 1

# Palette CRCs for BG (DESIGN §5.4): with the Get_color8 clamp, and with a (wrong) uint8 wrap.
BG_PALETTE0_CRC32 = 0xC9C2C0E7
BG_PALETTE0_CRC32_WRAPPED = 0x78D19732


def crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC (zlib, PNG): poly 0xEDB88320 reflected, init/xorout 0xFFFFFFFF."""
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def crc32_hex(data: bytes) -> str:
    return f"{crc32(data):08x}"


def palette_crc32(pal8: np.ndarray) -> int:
    """CRC32 of the 768 bytes ``Pal8.rgb`` (palette 0, 8-bit, Get_color8 clamp)."""
    b = np.asarray(pal8, np.uint8).reshape(256, 3).tobytes()
    assert len(b) == 768
    return crc32(b)


def fnv1a64(data: bytes, h: int = FNV64_OFFSET) -> int:
    """FNV-1a 64-bit (offset 0xcbf29ce484222325, prime 0x100000001b3); about 2 ms per 16 KiB."""
    mask = 0xFFFFFFFFFFFFFFFF
    for b in bytes(data):
        h = ((h ^ b) * FNV64_PRIME) & mask
    return h


def t1_key(kinds_is_flat: np.ndarray, flats: list[np.ndarray | None]) -> int:
    """T1 terrain key (DESIGN §5.2): FNV-1a-64 over ``"U7TK" | 0x01 | own[32] | pix[16384]``.

    ``kinds_is_flat``: (256,) bool, True where tile t (row-major) resolves to a non-RLE frame.
    ``flats``: 256 entries, the 8x8 flat of tile t (used only where ``kinds_is_flat``).
    ``own`` is a 256-bit map, LSB first; ``pix`` is the 128x128 raster with own cells holding
    their 64 flat pixels and every other cell zeros. Independent of the under-RLE fill.
    """
    own = bytearray(32)
    pix = np.zeros((128, 128), np.uint8)
    for t in range(256):
        if kinds_is_flat[t]:
            own[t >> 3] |= 1 << (t & 7)
            ty, tx = divmod(t, 16)
            pix[ty * 8:(ty + 1) * 8, tx * 8:(tx + 1) * 8] = flats[t]
    return fnv1a64(T1_MAGIC + bytes([T1_VERSION]) + bytes(own) + pix.tobytes())


def t1_hex(key: int) -> str:
    return f"{key:016x}"
