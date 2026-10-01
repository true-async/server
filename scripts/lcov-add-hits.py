#!/usr/bin/env python3
"""Add the hit counts of one lcov tracefile to the lines of another.

The base tracefile decides what is counted: its files, its lines, its
functions. The extra tracefile only adds hits to them. A line or a function
that exists only in the extra one is dropped.

Why not `lcov --add-tracefile`: it takes the union of the line tables. The
cmocka suite compiles the same sources without -O2 and with test-only
macros, so the union counts lines the shipped extension does not have, and
a file both builds share reads lower than the extension alone made it.

Branch records of the base are kept as they are.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def read_hits(path: Path) -> dict[str, tuple[dict[int, int], dict[str, int]]]:
    """Per source file: hits per line (DA) and per function name (FNDA)."""
    files: dict[str, tuple[dict[int, int], dict[str, int]]] = {}
    lines: dict[int, int] = {}
    funcs: dict[str, int] = {}
    for raw in path.read_text().splitlines():
        if raw.startswith("SF:"):
            lines, funcs = files.setdefault(raw[3:], ({}, {}))
        elif raw.startswith("DA:"):
            no, count = raw[3:].split(",")[:2]
            lines[int(no)] = lines.get(int(no), 0) + int(count)
        elif raw.startswith("FNDA:"):
            count, name = raw[5:].split(",", 1)
            funcs[name] = funcs.get(name, 0) + int(count)
    return files


def merge(base: Path, extra: Path) -> str:
    extra_hits = read_hits(extra)
    out: list[str] = []
    lines: dict[int, int] = {}
    funcs: dict[str, int] = {}

    for raw in base.read_text().splitlines():
        if raw.startswith("SF:"):
            lines, funcs = extra_hits.get(raw[3:], ({}, {}))
            hit_lines = 0
            hit_funcs = 0
            out.append(raw)
        elif raw.startswith("DA:"):
            fields = raw[3:].split(",")
            count = int(fields[1]) + lines.get(int(fields[0]), 0)
            hit_lines += count > 0
            out.append("DA:" + ",".join([fields[0], str(count)] + fields[2:]))
        elif raw.startswith("FNDA:"):
            count, name = raw[5:].split(",", 1)
            count = int(count) + funcs.get(name, 0)
            hit_funcs += count > 0
            out.append(f"FNDA:{count},{name}")
        elif raw.startswith("LH:"):
            out.append(f"LH:{hit_lines}")
        elif raw.startswith("FNH:"):
            out.append(f"FNH:{hit_funcs}")
        else:
            out.append(raw)

    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("base", type=Path, help="tracefile whose lines are counted")
    ap.add_argument("extra", type=Path, help="tracefile whose hits are added")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    for path in (args.base, args.extra):
        if not path.exists():
            print(f"error: {path} not found", file=sys.stderr)
            return 2

    args.output.write_text(merge(args.base, args.extra))
    return 0


if __name__ == "__main__":
    sys.exit(main())
