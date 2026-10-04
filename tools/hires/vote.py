#!/usr/bin/env python3
"""Majority voting over redundant runs: candidate sets, directory trees, pack-vs-candidate verification.

Thin wrapper around u7hires.vote.main(); run with --help for options.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from u7hires.vote import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
