#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("polybench")
s.require_checkout()
poly = s.here / "polybench"

# kernel -> (path, bench dataset)
# default is LARGE, the cubic ones get MEDIUM
KERNELS = {
    "gemm": ("linear-algebra/blas/gemm", None),
    "3mm": ("linear-algebra/kernels/3mm", "-DMEDIUM_DATASET"),
    "jacobi-2d": ("stencils/jacobi-2d", None),
    "floyd-warshall": ("medley/floyd-warshall", "-DMEDIUM_DATASET"),
    "correlation": ("datamining/correlation", None),
}


def build(name, defines, reference=False):
    kdir = poly / KERNELS[name][0]
    srcs = [poly / "utilities/polybench.c", kdir / f"{name}.c"]
    if reference:
        exe = s.dir / f"{name}-ref"
        cmd = ["gcc", "-O1", "-w", "-ffp-contract=off", f"-I{poly}/utilities", f"-I{kdir}", *defines, *map(str, srcs), "-o", str(exe), "-lm"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        s.log(result.stdout + result.stderr)
        if result.returncode != 0:
            raise lib.BuildError
        return exe
    s.flags = [f"-I{poly}/utilities", f"-I{kdir}", *defines]
    s.compile(f"obj-{name}", srcs)
    return s.link(name, sorted((s.dir / f"obj-{name}").glob("*.o")))


def dump(exe, out):
    with open(out, "wb") as arrays:
        return subprocess.run([str(exe)], stdout=subprocess.DEVNULL, stderr=arrays).returncode


def suite():
    s.ldlibs = ["-lm"]
    for name in KERNELS:
        defines = ["-DMINI_DATASET", "-DPOLYBENCH_DUMP_ARRAYS"]
        try:
            mine = build(name, defines)
            ref = build(name, defines, reference=True)
        except lib.BuildError:
            s.fail(name, f"build, see {s.dir}/build.log")
            continue
        ours, theirs = s.dir / f"{name}.dump", s.dir / f"{name}-ref.dump"
        if dump(mine, ours) == 0 and dump(ref, theirs) == 0 and ours.read_bytes() == theirs.read_bytes():
            s.ok(name)
        else:
            s.fail(name, "arrays differ from the gcc reference")


def bench():
    results = {}
    for cc in ("rat", *lib.host_compilers()):
        s.use(cc)
        s.ldlibs = ["-lm"]
        for name, (_, dataset) in KERNELS.items():
            defines = ["-DPOLYBENCH_TIME"] + ([dataset] if dataset else [])
            try:
                exe = build(name, defines)
            except lib.BuildError:
                s.fail(f"{cc} {name}", f"build, see {s.dir}/build.log")
                continue
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            try:
                results.setdefault(name, {})[cc] = round(float(result.stdout), 3)
            except ValueError:
                s.fail(f"{cc} {name}", "no timing output")
    if not s.quiet:
        lib.bench_header()
    s.compare(results)


s.for_levels(bench if s.bench_mode else suite)
