#!/usr/bin/env python3
"""QA metrics A1-A3 B1-B4 C1 C2 D1, report, contact sheets and 6x previews (DESIGN §8.3).

Thin wrapper around u7hires.qa.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.qa import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
