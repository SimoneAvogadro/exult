"""Pixel-art scalers used by the routes: xBRZ 1.9 and MMPX through ctypes, Scale2x/Scale3x in NumPy.

The shared libraries are built outside the repository (``third_party/build_xbrz.sh``); their paths
come from ``U7_XBRZ_LIB`` / ``U7_MMPX_LIB`` or the defaults below.
"""

from __future__ import annotations

import ctypes
import os
from functools import lru_cache

import numpy as np

DEFAULT_XBRZ_LIB = "/home/simonea/ultima7_exult/tmp/algo/xbrz19/libxbrz19.so"
DEFAULT_MMPX_LIB = "/home/simonea/ultima7_exult/tmp/algo/mmpx/libmmpx.so"
XBRZ_PATCHED_SOURCE_SHA256 = "247efab3a99b21ba635df6f991d26970309f917bfaec59d3629404c3f8187f76"
XBRZ_DEFAULT_CFG = {"eq_tol": 30.0, "center_bias": 4.0, "dominant": 3.6, "steep": 2.4}


def xbrz_lib_path() -> str:
    return os.environ.get("U7_XBRZ_LIB", DEFAULT_XBRZ_LIB)


def mmpx_lib_path() -> str:
    return os.environ.get("U7_MMPX_LIB", DEFAULT_MMPX_LIB)


def xbrz_available() -> bool:
    return os.path.exists(xbrz_lib_path())


@lru_cache(maxsize=None)
def _xbrz():
    lib = ctypes.CDLL(xbrz_lib_path())
    lib.xbrz_scale.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_double]
    lib.xbrz_scale.restype = None
    return lib


@lru_cache(maxsize=None)
def _mmpx():
    lib = ctypes.CDLL(mmpx_lib_path())
    lib.mmpx_scale2x.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    lib.mmpx_scale2x.restype = None
    return lib


def pack_rgb(rgb: np.ndarray) -> np.ndarray:
    rgb = np.asarray(rgb, np.uint8)
    return (rgb[..., 0].astype(np.uint32) << 16) | (rgb[..., 1].astype(np.uint32) << 8) | rgb[..., 2]


def unpack_rgb(p: np.ndarray) -> np.ndarray:
    return np.stack([(p >> 16) & 255, (p >> 8) & 255, p & 255], -1).astype(np.uint8)


def xbrz_rgb(rgb: np.ndarray, factor: int = 6, eq_tol: float = 30.0, center_bias: float = 4.0,
             dominant: float = 3.6, steep: float = 2.4) -> np.ndarray:
    """(H,W,3) uint8 -> (H*f, W*f, 3) uint8 with xBRZ 1.9 in RGB mode. Deterministic."""
    rgb = np.asarray(rgb, np.uint8)
    H, W, _ = rgb.shape
    src = np.ascontiguousarray(pack_rgb(rgb))
    dst = np.zeros((H * factor, W * factor), np.uint32)
    _xbrz().xbrz_scale(int(factor), src.ctypes.data, dst.ctypes.data, W, H, 0,
                       float(eq_tol), float(center_bias), float(dominant), float(steep))
    return unpack_rgb(dst)


def mmpx2x_packed(packed: np.ndarray) -> np.ndarray:
    """MMPX 2x on packed 0x00RRGGBB colours (alpha byte 0 = opaque in the port's luma)."""
    packed = np.ascontiguousarray(packed, np.uint32)
    H, W = packed.shape
    dst = np.zeros((H * 2, W * 2), np.uint32)
    _mmpx().mmpx_scale2x(packed.ctypes.data, dst.ctypes.data, W, H)
    return dst


def scale2x(idx: np.ndarray, pad: str = "edge") -> np.ndarray:
    """AdvMAME2x/EPX on any equality-comparable plane (palette indices work directly)."""
    p = np.pad(idx, 1, mode=pad)
    B, D, E, Fv, Hh = p[:-2, 1:-1], p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:], p[2:, 1:-1]
    cond = (B != Hh) & (D != Fv)
    h, w = idx.shape
    out = np.empty((h * 2, w * 2), idx.dtype)
    out[0::2, 0::2] = np.where(cond & (D == B), D, E)
    out[0::2, 1::2] = np.where(cond & (B == Fv), Fv, E)
    out[1::2, 0::2] = np.where(cond & (D == Hh), D, E)
    out[1::2, 1::2] = np.where(cond & (Hh == Fv), Fv, E)
    return out


def scale3x(idx: np.ndarray, pad: str = "edge") -> np.ndarray:
    """AdvMAME3x on an index plane."""
    p = np.pad(idx, 1, mode=pad)
    A, B, C = p[:-2, :-2], p[:-2, 1:-1], p[:-2, 2:]
    D, E, Fv = p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:]
    G, Hh, I = p[2:, :-2], p[2:, 1:-1], p[2:, 2:]
    cond = (B != Hh) & (D != Fv)
    o = [
        np.where(cond & (D == B), D, E),
        np.where(cond & (((D == B) & (E != C)) | ((B == Fv) & (E != A))), B, E),
        np.where(cond & (B == Fv), Fv, E),
        np.where(cond & (((D == B) & (E != G)) | ((D == Hh) & (E != A))), D, E),
        E,
        np.where(cond & (((B == Fv) & (E != I)) | ((Hh == Fv) & (E != C))), Fv, E),
        np.where(cond & (D == Hh), D, E),
        np.where(cond & (((D == Hh) & (E != I)) | ((Hh == Fv) & (E != G))), Hh, E),
        np.where(cond & (Hh == Fv), Fv, E),
    ]
    h, w = idx.shape
    out = np.empty((h * 3, w * 3), idx.dtype)
    for k in range(9):
        out[k // 3::3, k % 3::3] = o[k]
    return out


def nearest(idx: np.ndarray, f: int) -> np.ndarray:
    return np.repeat(np.repeat(idx, f, 0), f, 1)
