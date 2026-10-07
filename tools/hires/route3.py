#!/usr/bin/env python3
"""Route 3: xBRZ 6x + local snap + mode consensus on CPU (DESIGN §8.2 A2).

Thin wrapper around u7hires.route3.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.route3 import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
