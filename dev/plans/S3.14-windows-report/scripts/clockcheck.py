"""Checks that time.perf_counter_ns() is one clock across processes on this
machine: a child's reading must fall between the parent's readings taken just
before the child starts and just after it exits. Prints one line per try and
exits non-zero on any miss."""
import subprocess
import sys
import time

TRIES = 20
bad = 0
for i in range(TRIES):
    before = time.perf_counter_ns()
    child = int(subprocess.check_output([sys.executable, "-c", "import time; print(time.perf_counter_ns())"]))
    after = time.perf_counter_ns()
    ok = before <= child <= after
    bad += not ok
    print(f"{i}: before={before} child={child} after={after} {'ok' if ok else 'MISS'}")
print(f"misses: {bad} of {TRIES}")
sys.exit(1 if bad else 0)
