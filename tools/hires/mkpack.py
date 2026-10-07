#!/usr/bin/env python3
"""Build a pack (loose PNG groups, sidecars, pack.txt, optional flats.bundle) from route candidates (DESIGN §5, §8.2 A3).

Thin wrapper around u7hires.pack.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.pack import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
