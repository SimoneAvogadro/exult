#!/usr/bin/env python3
"""Validate a pack with the engine's rules N1 F1 F2 F3 F4 P0 P4 G1 G2 R1 B0 plus offline P2/E1 (DESIGN §5.5).

Thin wrapper around u7hires.check.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.check import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
