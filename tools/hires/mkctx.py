#!/usr/bin/env python3
"""Build context windows per used terrain, macro sheets and self-wrap windows (DESIGN §8.2 A1).

Thin wrapper around u7hires.ctx.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.ctx import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
