#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

CORPUS = ("c99", "c-testsuite", "custom", "gcc-torture")

jobs, quiet, bench, picked = lib.parse_args(sys.argv[1:])
if bench:
    sys.exit(0)  # no benchmarks here
os.chdir(lib.ROOT)
here = Path("src/compiler/test/correctness")
runner = lib.tool("RUNNER", "bin/cc-test")

skips = set()
if not picked:
    for line in (here / "skip").read_text().splitlines():
        parts = line.split(None, 1)
        if not parts or parts[0].startswith("#"):
            continue
        case = here / parts[0]
        if case.is_file():
            skips.add(str(case))
            print(f"SKIP  {case}: {parts[1] if len(parts) > 1 else ''}", flush=True)
        else:
            print(f"skip: no such case '{parts[0]}'", file=sys.stderr)

cases = picked or sorted(str(case) for d in CORPUS for case in (here / d).rglob("*.c") if str(case) not in skips)
rc = subprocess.run([str(runner), f"-j{jobs}", *(["-q"] if quiet else []), *cases]).returncode
if not quiet:
    print(f"{len(skips)} skipped")
sys.exit(rc)
