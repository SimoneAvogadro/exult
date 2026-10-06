#!/usr/bin/env python3
"""Route 1: SDXL + tile ControlNet diffusion refine on GPU (Phase B pilot; needs venv-gpu).

Thin wrapper around u7hires.route1.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.route1 import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
