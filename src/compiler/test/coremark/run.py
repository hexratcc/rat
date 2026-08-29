#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("coremark")
s.require_checkout()
coremark = s.here / "coremark"
SRCS = ("core_list_join.c", "core_main.c", "core_matrix.c", "core_state.c", "core_util.c", "posix/core_portme.c")


def build():
    s.flags = [f"-I{coremark}", f"-I{coremark}/posix", "-DPERFORMANCE_RUN=1", "-DUSE_CLOCK=1", "-DITERATIONS=0", '-DFLAGS_STR="-O1"']
    s.compile(".", [coremark / src for src in SRCS])
    return s.link("coremark", sorted(s.dir.glob("*.o")))


def run(exe):
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    s.log(result.stdout + result.stderr)
    return result


def suite():
    result = run(build())
    if result.returncode == 0 and "Correct operation validated" in result.stdout:
        s.ok("validated")
    else:
        s.fail("validated", f"see {s.dir}/build.log")


def bench():
    results = {}
    for cc in ("rat", *lib.host_compilers()):
        s.use(cc)
        try:
            exe = build()
        except lib.BuildError:
            s.fail(f"{cc} build", f"see {s.dir}/build.log")
            continue
        def sample(exe=exe):
            result = run(exe)
            rate = re.search(r"Iterations/Sec   : ([\d.]+)", result.stdout)
            if result.returncode != 0 or not rate or "Correct operation validated" not in result.stdout:
                return None
            # seconds per 100k iterations, so lower is better like the rest
            return 100000 / float(rate.group(1))

        t = s.bench_min(sample)
        if t is None:
            s.fail(f"{cc} run", f"see {s.dir}/build.log")
        else:
            results.setdefault("coremark (s/100k iters)", {})[cc] = round(t, 3)
    if not s.quiet:
        lib.bench_header()
    s.compare(results)


s.for_levels(bench if s.bench_mode else suite)
