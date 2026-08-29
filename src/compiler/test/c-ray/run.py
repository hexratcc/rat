#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("c-ray")
s.require_checkout()
cray = s.here / "c-ray"


def build(reference=False):
    src = cray / "c-ray-f.c"
    if reference:
        exe = s.dir / "cray-ref"
        cmd = ["gcc", "-O1", "-w", "-ffp-contract=off", str(src), "-o", str(exe), "-lm"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        s.log(result.stdout + result.stderr)
        if result.returncode != 0:
            raise lib.BuildError
        return exe
    s.compile(".", [src])
    return s.link("cray", [s.dir / "c-ray-f.o"])


def render(exe, size, out):
    with open(cray / "scene") as scene:
        return subprocess.run([str(exe), "-s", size, "-o", str(out)], stdin=scene, capture_output=True, text=True)


def suite():
    s.ldlibs = ["-lm"]
    mine, ref = build(), build(reference=True)
    ours, theirs = s.dir / "out.ppm", s.dir / "ref.ppm"
    if render(mine, "800x600", ours).returncode == 0 and render(ref, "800x600", theirs).returncode == 0 and ours.read_bytes() == theirs.read_bytes():
        s.ok("render")
    else:
        s.fail("render", "image differs from the gcc reference")


def bench():
    results = {}
    for cc in ("rat", *lib.host_compilers()):
        s.use(cc)
        s.ldlibs = ["-lm"]
        try:
            exe = build()
        except lib.BuildError:
            s.fail(f"{cc} build", f"see {s.dir}/build.log")
            continue
        def sample(exe=exe):
            result = render(exe, "3200x2400", s.dir / "out.ppm")
            took = re.search(r"\((\d+) milliseconds\)", result.stderr)
            if result.returncode != 0 or not took:
                return None
            return int(took.group(1)) / 1000

        t = s.bench_min(sample)
        if t is None:
            s.fail(f"{cc} render", "no timing output")
        else:
            results.setdefault("render", {})[cc] = round(t, 3)
    if not s.quiet:
        lib.bench_header()
    s.compare(results)


s.for_levels(bench if s.bench_mode else suite)
