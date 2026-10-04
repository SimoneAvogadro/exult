"""Small shared helpers: logging, atomic writes, deterministic worker pools."""

from __future__ import annotations

import json
import logging
import os
import sys
import time
from contextlib import contextmanager

import numpy as np

MAX_WORKERS = 8   # another workflow builds C++ concurrently; never use more processes


def get_logger(name: str = "u7hires") -> logging.Logger:
    log = logging.getLogger(name)
    if not log.handlers:
        h = logging.StreamHandler(sys.stderr)
        h.setFormatter(logging.Formatter("%(asctime)s %(name)s %(levelname)s %(message)s", "%H:%M:%S"))
        log.addHandler(h)
        log.setLevel(os.environ.get("U7HIRES_LOG", "INFO"))
        log.propagate = False
    return log


def clamp_workers(n: int | None) -> int:
    if n is None or n <= 0:
        n = min(MAX_WORKERS, os.cpu_count() or 1)
    return max(1, min(int(n), MAX_WORKERS))


def single_thread_env() -> None:
    """Keep BLAS/OpenMP single-threaded inside worker processes."""
    for v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
        os.environ[v] = "1"


def atomic_write_bytes(path: str, data: bytes, fsync: bool = False) -> None:
    """Write ``path`` via ``path.tmp`` + rename, so readers never see a partial file.
    Skips the rewrite when the file already holds exactly ``data`` (keeps mtimes stable)."""
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    try:
        if os.path.getsize(path) == len(data):
            with open(path, "rb") as f:
                if f.read() == data:
                    return
    except OSError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        if fsync:
            f.flush()
            os.fsync(f.fileno())
    os.replace(tmp, path)


def to_jsonable(o):
    if isinstance(o, dict):
        return {str(k): to_jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [to_jsonable(v) for v in o]
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.floating):
        return float(o)
    if isinstance(o, np.ndarray):
        return to_jsonable(o.tolist())
    if isinstance(o, float) and (o != o):
        return None
    return o


def json_dumps(o, indent: int | None = 1) -> str:
    return json.dumps(to_jsonable(o), indent=indent, sort_keys=True)


def atomic_write_json(path: str, o, indent: int | None = 1) -> None:
    atomic_write_bytes(path, (json_dumps(o, indent) + "\n").encode())


def touch(path: str) -> None:
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "a"):
        pass
    os.utime(path, None)


class Progress:
    """Rate-limited progress logging."""

    def __init__(self, log: logging.Logger, total: int, what: str, every_s: float = 5.0):
        self.log, self.total, self.what, self.every = log, total, what, every_s
        self.n = 0
        self.t0 = self.last = time.time()

    def step(self, k: int = 1) -> None:
        self.n += k
        now = time.time()
        if now - self.last >= self.every or self.n == self.total:
            rate = self.n / max(1e-9, now - self.t0)
            eta = (self.total - self.n) / rate if rate > 0 else 0
            self.log.info("%s %d/%d (%.1f/s, eta %.0fs)", self.what, self.n, self.total, rate, eta)
            self.last = now


@contextmanager
def timed(log: logging.Logger, what: str):
    t0 = time.time()
    yield
    log.info("%s: %.2fs", what, time.time() - t0)


def key_name(shape: int, frame: int) -> str:
    return f"{shape:04d}_{frame:02d}"
