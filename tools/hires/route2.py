#!/usr/bin/env python3
"""Route 2: spandrel 4x-NXbrz on GPU + Lanczos 1.5x + colour lock + snap + consensus (DESIGN §8.4 B1). Run with the CUDA venv.

Thin wrapper around u7hires.route2.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.route2 import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
