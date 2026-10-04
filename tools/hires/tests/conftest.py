import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # tools/hires (the u7hires package)
sys.path.insert(0, HERE)                    # synth helpers

DATA = os.path.join(HERE, "data")


@pytest.fixture(scope="session")
def synth_static(tmp_path_factory):
    import synth
    return synth.write_static(str(tmp_path_factory.mktemp("static")))


@pytest.fixture(scope="session")
def world(synth_static):
    from u7hires.io import World
    return World.load(synth_static)


@pytest.fixture(scope="session")
def provider(world):
    from u7hires.check import Provider
    return Provider.from_world(world)


def real_static_dir():
    """The real BG STATIC dir, or None (then the smoke tests skip). U7HIRES_NO_REAL_DATA=1 forces None."""
    if os.environ.get("U7HIRES_NO_REAL_DATA"):
        return None
    from u7hires.io import default_static_dir
    return default_static_dir()
