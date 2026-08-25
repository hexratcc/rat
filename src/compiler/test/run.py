#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib

os.chdir(lib.ROOT)
TEST = Path("src/compiler/test")
LOG = Path("build/test/results.log")

for exe in ("bin/rat-test", "bin/cc"):
    if not os.access(exe, os.X_OK):
        sys.exit("missing binaries, run make")

names = sys.argv[1:]
bench = "bench" in names
names = [name for name in names if name != "bench"]
run_ir = not names and not bench
if not names:
    names = sorted(child.name for child in TEST.iterdir() if (child / "run.py").is_file())

jobs = os.cpu_count()
counts = {"PASS": 0, "FAIL": 0, "SKIP": 0, "BENCH": 0}
totals = {}


def report(cmd, cwd=None):
    with open(LOG, "a") as log:
        child = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, text=True)
        for line in child.stdout:
            sys.stdout.write(line)
            log.write(line)
            word = line.split(" ", 1)[0]
            if word in counts:
                counts[word] += 1
            if word == "BENCH":  # the three value columns end every row
                for cc, value in zip(("rat", "gcc", "clang"), line.split()[-3:]):
                    if value != "-":
                        totals[cc] = totals.get(cc, 0) + float(value)
    return child.wait() == 0


LOG.parent.mkdir(parents=True, exist_ok=True)
LOG.write_text("")
ok = True
if bench:
    lib.bench_header()
if run_ir:
    ok &= report(["../../bin/rat-test", f"-j{jobs}", "-q"], cwd="src/backend")
for name in names:
    runner = TEST / name / "run.py"
    if not runner.is_file():
        sys.exit(f"no such suite: {name}")
    ok &= report([sys.executable, str(runner), f"-j{jobs}", "-q"] + (["bench"] if bench else []))

if bench:
    if counts["BENCH"]:
        lib.bench_total(totals)
    print(f"\n{counts['BENCH']} benchmarks, {counts['FAIL']} failed")
else:
    print(f"\n{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped")
sys.exit(0 if ok and not counts["FAIL"] else 1)
