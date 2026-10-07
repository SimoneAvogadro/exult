#!/usr/bin/env python3
"""Whole-terrain override (x6/terrain/<t1>.png, WP-17) from a route-1 diffusion window.

Thin wrapper around u7hires.terrainov.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.terrainov import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
