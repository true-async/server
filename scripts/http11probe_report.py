#!/usr/bin/env python3
"""Render an Http11Probe results JSON into a committed markdown report.

Usage: http11probe_report.py <results.json> <out.md>

The JSON is produced by `Http11Probe.Cli --output`. The report groups
verdicts (Pass / Warn / Fail) by category and lists every non-pass check
with its RFC level, expectation, and what the server actually returned —
so the committed file is a human-readable conformance snapshot, like
docs/coverage-baseline.json is for coverage.
"""
import json
import sys
import datetime


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: http11probe_report.py <results.json> <out.md>", file=sys.stderr)
        return 2

    data = json.load(open(sys.argv[1]))
    summary = data.get("summary", {})
    results = data.get("results", [])

    by_verdict = {"Fail": [], "Warn": [], "Pass": []}
    for r in results:
        by_verdict.setdefault(r.get("verdict", "?"), []).append(r)

    out = []
    out.append("# Http11Probe conformance report")
    out.append("")
    out.append("HTTP/1.1 RFC 9110/9112 compliance + request-smuggling + malformed-input "
               "+ header-normalization + caching/cookie probe "
               "([MDA2AV/Http11Probe](https://github.com/MDA2AV/Http11Probe)).")
    out.append("")
    out.append(f"_Generated: {datetime.datetime.now(datetime.UTC).strftime('%Y-%m-%d %H:%M UTC')} "
               "— refreshed weekly by `.github/workflows/chaos.yml`._")
    out.append("")
    out.append("## Summary")
    out.append("")
    out.append("| Total | Scored | Passed | Warnings | Failed | Errors |")
    out.append("|------:|-------:|-------:|---------:|-------:|-------:|")
    out.append(f"| {summary.get('total','?')} | {summary.get('scored','?')} "
               f"| {summary.get('passed','?')} | {summary.get('warnings','?')} "
               f"| {summary.get('failed','?')} | {summary.get('errors','?')} |")
    out.append("")

    def section(title: str, rows: list) -> None:
        out.append(f"## {title} ({len(rows)})")
        out.append("")
        if not rows:
            out.append("_None._")
            out.append("")
            return
        out.append("| Category | Check | RFC level | Expected | Got |")
        out.append("|----------|-------|-----------|----------|-----|")
        for r in sorted(rows, key=lambda x: (x.get("category", ""), x.get("id", ""))):
            got = f"status={r.get('statusCode')} conn={r.get('connectionState')}"
            exp = str(r.get("expected", "")).replace("|", "\\|")
            out.append(f"| {r.get('category','')} | `{r.get('id','')}` "
                       f"| {r.get('rfcLevel','')} | {exp} | {got} |")
        out.append("")

    section("Failures", by_verdict["Fail"])
    section("Warnings", by_verdict["Warn"])

    open(sys.argv[2], "w").write("\n".join(out) + "\n")
    print(f"wrote {sys.argv[2]}: "
          f"{summary.get('passed')} pass / {summary.get('warnings')} warn / "
          f"{summary.get('failed')} fail")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
