#!/usr/bin/env python3
"""Side-by-side comparison of flat packs: per-family sheets, sparkle sheets, 6x view crops, previews, metrics.

Thin wrapper around u7hires.compare.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.compare import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
