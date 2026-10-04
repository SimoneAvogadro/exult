"""Smoke test of the Python environment of the hi-res tools (tools-venv).

The pinned versions of tools/hires/requirements.txt are the ones installed, and
an indexed PNG keeps its palette indices through a write and a read with
Pillow, as the art pipeline relies on (DESIGN.md section 5.3). Synthetic data
only.
"""

import io
import os
import re
from importlib import metadata

import numpy as np
import pytest
from PIL import Image

REQUIREMENTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "requirements.txt")


def pinned_requirements():
    pins = {}
    with open(REQUIREMENTS, encoding="utf-8") as req:
        for line in req:
            match = re.match(r"^\s*([A-Za-z0-9_.-]+)==([^\s#]+)", line)
            if match:
                pins[match.group(1)] = match.group(2)
    return pins


def test_requirements_pin_pytest_numpy_pillow():
    assert {"numpy", "pillow", "pytest"} <= set(pinned_requirements())


@pytest.mark.parametrize("name,version", sorted(pinned_requirements().items()))
def test_installed_version_matches_pin(name, version):
    assert metadata.version(name) == version


def test_indexed_png_round_trip():
    width, height = 48, 32
    indices = ((np.arange(width * height, dtype=np.uint32) * 2654435761) >> 24).astype(np.uint8)
    indices = indices.reshape(height, width)
    palette = [(i * 37 + c * 91) % 256 for i in range(256) for c in range(3)]

    image = Image.frombytes("P", (width, height), indices.tobytes())
    image.putpalette(palette)
    data = io.BytesIO()
    image.save(data, format="PNG")

    data.seek(0)
    with Image.open(data) as back:
        assert back.mode == "P"
        assert back.getpalette()[: 3 * 256] == palette
        assert np.array_equal(np.asarray(back), indices)
