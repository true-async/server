"""Turns results-*.jsonl into one Markdown table row per cell: min and max of the
times, the min-max of each worker slot's total_requests (slot = position in
getStats()['workers']), the min-max of the largest share whatever slot held it,
and every run that recorded a problem.

Usage: summarize.py <results.jsonl>...
"""
import json
import sys
from collections import defaultdict


def rng(values):
    return f"{min(values)} … {max(values)}" if values else "—"


def main():
    cells = defaultdict(list)
    for path in sys.argv[1:]:
        with open(path) as f:
            for line in f:
                r = json.loads(line)
                cells[(r["conns"], r["build"], r["mode"], r["workers"])].append(r)

    print("| conns | build | config | workers | runs | time, ms | share per slot | largest share | problems |")
    print("|---|---|---|---|---|---|---|---|---|")
    for key in sorted(cells, key=lambda k: (k[0], k[3], k[2] != "uncapped", k[1] != "one")):
        runs = cells[key]
        times = [round(r["time_ms"]) for r in runs if "time_ms" in r]
        slots = defaultdict(list)
        largest = []
        for r in runs:
            shares = r.get("stats", {}).get("workers")
            if shares is None:
                continue
            shares = list(shares.values()) if isinstance(shares, dict) else shares
            for i, s in enumerate(shares):
                slots[i].append(s)
            largest.append(max(shares))
        per_slot = "; ".join(f"w{i}: {rng(v)}" for i, v in sorted(slots.items()))
        problems = [f"seq {r['seq']}: {', '.join(r['problems'])}" for r in runs if r["problems"]]
        conns, build, mode, workers = key
        print(f"| {conns} | {build} | {mode} | {workers} | {len(runs)} | {rng(times)} | {per_slot} | "
              f"{rng(largest)} | {'; '.join(problems) or 'none'} |")

    print()
    print("| seq | round | conns | build | config | workers | time, ms | total_requests per slot | 200s |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in sorted((r for runs in cells.values() for r in runs), key=lambda r: r["seq"]):
        shares = r.get("stats", {}).get("workers")
        shares = list(shares.values()) if isinstance(shares, dict) else shares
        print(f"| {r['seq']} | {r['round']} | {r['conns']} | {r['build']} | {r['mode']} | {r['workers']} | "
              f"{r.get('time_ms', 0):.1f} | {shares} | {r.get('ok_200')} |")


if __name__ == "__main__":
    main()
