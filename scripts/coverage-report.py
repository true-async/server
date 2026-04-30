#!/usr/bin/env python3
"""Convert lcov filtered.info to a stable per-file JSON summary.

Output schema:
{
  "totals": {"lines": {"total": int, "hit": int}, "functions": {...}},
  "files":  {"<repo-relative path>": {"lines": {...}, "functions": {...}}}
}

Repo-relative paths use forward slashes regardless of platform so the
JSON is stable across runners and diffable with git.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def parse_info(info_path: Path, repo_root: Path) -> dict:
    files: dict[str, dict] = {}
    cur_path: str | None = None
    cur: dict | None = None

    for raw in info_path.read_text().splitlines():
        line = raw.strip()
        if line.startswith("SF:"):
            abs_path = Path(line[3:]).resolve()
            try:
                rel = abs_path.relative_to(repo_root.resolve()).as_posix()
            except ValueError:
                rel = abs_path.as_posix()
            cur_path = rel
            cur = {"lines": {"total": 0, "hit": 0},
                   "functions": {"total": 0, "hit": 0}}
        elif cur is None:
            continue
        elif line.startswith("LF:"):
            cur["lines"]["total"] = int(line[3:])
        elif line.startswith("LH:"):
            cur["lines"]["hit"] = int(line[3:])
        elif line.startswith("FNF:"):
            cur["functions"]["total"] = int(line[4:])
        elif line.startswith("FNH:"):
            cur["functions"]["hit"] = int(line[4:])
        elif line == "end_of_record":
            assert cur_path is not None
            files[cur_path] = cur
            cur_path, cur = None, None

    totals = {"lines": {"total": 0, "hit": 0},
              "functions": {"total": 0, "hit": 0}}
    for f in files.values():
        for k in ("lines", "functions"):
            totals[k]["total"] += f[k]["total"]
            totals[k]["hit"] += f[k]["hit"]

    return {"totals": totals, "files": dict(sorted(files.items()))}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("info", type=Path, help="path to lcov .info")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--repo-root", type=Path,
                    default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    if not args.info.exists():
        print(f"error: {args.info} not found", file=sys.stderr)
        return 2

    report = parse_info(args.info, args.repo_root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {args.output} ({len(report['files'])} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
