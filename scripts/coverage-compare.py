#!/usr/bin/env python3
"""Compare two coverage JSON reports and emit a markdown delta.

Exit codes:
  0 — no regression beyond threshold (or `[coverage-drop-ok]` opt-out)
  1 — regression in a touched file beyond threshold
  2 — invocation error
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

THRESHOLD_PP = 1.0  # max permitted line-coverage drop in pp per touched file


def pct(hit: int, total: int) -> float:
    return 100.0 * hit / total if total else 0.0


def fmt_pct(p: float) -> str:
    return f"{p:.2f}%"


def fmt_delta(d: float) -> str:
    sign = "+" if d >= 0 else ""
    return f"{sign}{d:.2f} pp"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--current", type=Path, required=True)
    ap.add_argument("--touched", type=Path,
                    help="newline-separated list of repo-relative paths "
                         "modified in this PR; only these gate the run")
    ap.add_argument("--allow-drop", action="store_true",
                    help="set when commit message contains [coverage-drop-ok]")
    ap.add_argument("--output", type=Path,
                    help="write markdown report to this path")
    args = ap.parse_args()

    cur = json.loads(args.current.read_text())

    base_raw = (json.loads(args.baseline.read_text())
                if args.baseline.exists() and args.baseline.stat().st_size > 0
                else None)
    if not base_raw or "files" not in base_raw or "totals" not in base_raw:
        msg = ("## Coverage\n\n"
               "_No baseline yet — recording this run as the new baseline._\n\n"
               f"Total lines: **{fmt_pct(pct(cur['totals']['lines']['hit'], cur['totals']['lines']['total']))}**, "
               f"functions: **{fmt_pct(pct(cur['totals']['functions']['hit'], cur['totals']['functions']['total']))}** "
               f"across {len(cur['files'])} files.\n")
        if args.output:
            args.output.write_text(msg)
        else:
            print(msg)
        return 0

    base = base_raw
    base_files = base.get("files", {})
    cur_files = cur["files"]

    touched: set[str] = set()
    if args.touched and args.touched.exists():
        touched = {ln.strip() for ln in args.touched.read_text().splitlines()
                   if ln.strip()}

    rows: list[tuple[str, float, float, float, bool]] = []
    regressions: list[tuple[str, float]] = []

    all_paths = sorted(set(base_files) | set(cur_files))
    for path in all_paths:
        b = base_files.get(path, {"lines": {"total": 0, "hit": 0}})
        c = cur_files.get(path, {"lines": {"total": 0, "hit": 0}})
        bp = pct(b["lines"]["hit"], b["lines"]["total"])
        cp = pct(c["lines"]["hit"], c["lines"]["total"])
        d = cp - bp
        is_touched = path in touched
        if abs(d) >= 0.01 or is_touched:
            rows.append((path, bp, cp, d, is_touched))
        if is_touched and d < -THRESHOLD_PP:
            regressions.append((path, d))

    bt = base["totals"]["lines"]
    ct = cur["totals"]["lines"]
    base_total = pct(bt["hit"], bt["total"])
    cur_total = pct(ct["hit"], ct["total"])
    total_delta = cur_total - base_total

    lines = ["## Coverage",
             "",
             f"**Total lines:** {fmt_pct(base_total)} → "
             f"{fmt_pct(cur_total)} ({fmt_delta(total_delta)})",
             ""]

    if rows:
        lines.append("| File | Baseline | Current | Δ | Touched |")
        lines.append("|---|---:|---:|---:|:---:|")
        for path, bp, cp, d, t in rows:
            mark = "●" if t else ""
            lines.append(
                f"| `{path}` | {fmt_pct(bp)} | {fmt_pct(cp)} | "
                f"{fmt_delta(d)} | {mark} |")
    else:
        lines.append("_No per-file changes._")

    if regressions and not args.allow_drop:
        lines.append("")
        lines.append(
            f"### ❌ Regression in touched files (> {THRESHOLD_PP} pp drop)")
        for p, d in regressions:
            lines.append(f"- `{p}` dropped {fmt_delta(d)}")
        lines.append("")
        lines.append("Add `[coverage-drop-ok]` to a commit message in this "
                     "PR to override.")

    md = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(md)
    else:
        print(md)

    if regressions and not args.allow_drop:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
