"""S3.14 harness: runs the burst matrix against the two builds and appends one
JSON line per run to the results file.

Every run starts a fresh server, waits for its ready file, arms the clients,
releases them together through one go file, then hands the server the number
of answered requests and collects its per-worker stats. Nothing is dropped: a
run with a refused connect, a non-200 answer, an unfinished connection or a
server that did not exit cleanly is recorded with what went wrong.

Usage: run.py main <results.jsonl>   the 9-cell matrix, 7 rounds
       run.py big  <results.jsonl>   one / uncapped / 1 worker, 2000 connects, 7 runs
"""
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILDS = {
    "one": os.path.join(ROOT, "obj-one", "Release_TS", "php.exe"),
    "many": os.path.join(ROOT, "obj-many", "Release_TS", "php.exe"),
}
WORK = os.path.join(ROOT, "runs")
PER_CLIENT = 200
ROUNDS = 7
WORKERS = (1, 2, 4)
# One round: the A/B/A order of the task, per worker count.
ROUND_CELLS = (("one", "uncapped"), ("many", "uncapped"), ("one", "capped"))
BASE_PORT = 21000
READY_TIMEOUT_S = 30
ARM_TIMEOUT_S = 15
CLIENT_TIMEOUT_S = 60
STATS_TIMEOUT_S = 15
EXIT_TIMEOUT_S = 15
PAUSE_S = 1.0


def wait_for(path, timeout, proc=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.005)
    return False


def one_run(seq, build, mode, workers, conns):
    ctl = os.path.join(WORK, f"run-{seq:04d}")
    shutil.rmtree(ctl, ignore_errors=True)
    os.makedirs(ctl)
    port = BASE_PORT + seq
    env = {k: v for k, v in os.environ.items() if k != "CODEL_TARGET_MS"}
    rec = {"seq": seq, "build": build, "mode": mode, "workers": workers, "conns": conns,
           "port": port, "problems": []}

    log = open(os.path.join(ctl, "server.log"), "wb")
    srv = subprocess.Popen([BUILDS[build], "-n", os.path.join(HERE, "server.php"), str(port), str(workers), mode, ctl],
                           stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        if not wait_for(os.path.join(ctl, "ready"), READY_TIMEOUT_S, srv):
            rec["problems"].append(f"server not ready, exit={srv.poll()}")
            return rec
        with open(os.path.join(ctl, "ready")) as f:
            rec["server_config"] = json.load(f)

        go = os.path.join(ctl, "go")
        outs = [os.path.join(ctl, f"client-{i}.json") for i in range(conns // PER_CLIENT)]
        clients = [subprocess.Popen([sys.executable, os.path.join(HERE, "client.py"), str(port), str(PER_CLIENT), go, o])
                   for o in outs]
        for o in outs:
            if not wait_for(o + ".armed", ARM_TIMEOUT_S):
                rec["problems"].append(f"client not armed: {o}")
        open(go, "w").close()

        for c in clients:
            try:
                c.wait(CLIENT_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                c.kill()
                rec["problems"].append("client killed on timeout")

        results = []
        for o in outs:
            if os.path.exists(o):
                with open(o) as f:
                    results.append(json.load(f))
            else:
                rec["problems"].append(f"no client output: {o}")

        statuses, errors, unfinished = {}, [], 0
        for r in results:
            for line, n in r["statuses"].items():
                statuses[line] = statuses.get(line, 0) + n
            errors += r["errors"]
            unfinished += r["unfinished"]
        rec["time_ms"] = (max(r["last_done_ns"] for r in results) - min(r["first_connect_ns"] for r in results)) / 1e6
        rec["start_skew_ms"] = (max(r["first_connect_ns"] for r in results) -
                                min(r["first_connect_ns"] for r in results)) / 1e6
        rec["statuses"] = statuses
        rec["errors"] = errors
        rec["unfinished"] = unfinished
        ok = sum(n for line, n in statuses.items() if line.split(" ")[1:2] == ["200"])
        rec["ok_200"] = ok
        if ok != conns:
            rec["problems"].append(f"{ok} of {conns} answered 200")

        # The server polls for this file and reads it at once: write aside, then rename.
        with open(os.path.join(ctl, "done.tmp"), "w") as f:
            f.write(str(sum(statuses.values())))
        os.replace(os.path.join(ctl, "done.tmp"), os.path.join(ctl, "done"))
        if wait_for(os.path.join(ctl, "stats.json"), STATS_TIMEOUT_S, srv):
            with open(os.path.join(ctl, "stats.json")) as f:
                rec["stats"] = json.load(f)
        else:
            rec["problems"].append("no stats.json")
    finally:
        try:
            rec["server_exit"] = srv.wait(EXIT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            srv.kill()
            rec["server_exit"] = "killed"
            rec["problems"].append("server killed on exit timeout")
        log.close()
        with open(os.path.join(ctl, "server.log"), "rb") as f:
            tail = f.read().decode("utf-8", "replace").strip()
        if tail:
            rec["server_log"] = tail[-2000:]
    return rec


def main():
    plan, out = sys.argv[1], sys.argv[2]
    if plan == "main":
        runs = [(b, m, w, 1000, r) for r in range(ROUNDS) for w in WORKERS for b, m in ROUND_CELLS]
        seq0 = 0
    elif plan == "big":
        runs = [("one", "uncapped", 1, 2000, r) for r in range(ROUNDS)]
        seq0 = 500
    else:
        sys.exit(f"unknown plan {plan}")

    os.makedirs(WORK, exist_ok=True)
    for i, (build, mode, workers, conns, rnd) in enumerate(runs):
        rec = one_run(seq0 + i, build, mode, workers, conns)
        rec["round"] = rnd
        rec["wall_clock"] = time.strftime("%Y-%m-%d %H:%M:%S")
        with open(out, "a") as f:
            f.write(json.dumps(rec) + "\n")
        shares = rec.get("stats", {}).get("workers")
        print(f"{rec['seq']} {build}/{mode}/{workers}w/{conns}: {rec.get('time_ms')} ms "
              f"shares={shares} problems={rec['problems']}", flush=True)
        time.sleep(PAUSE_S)


if __name__ == "__main__":
    main()
