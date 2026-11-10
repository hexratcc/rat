#!/usr/bin/env python3
# each <name>.c is built with -g (or the flags of a leading "// flags:" line) and linked by bin/link.
# gdb then runs <name>.gdb, and each line of <name>.<level>.expect must show up in its output, in
# order, several may share one output line
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("debug")
if s.bench_mode:
    sys.exit(0)  # no benchmarks here
gdb = shutil.which("gdb")
ratlink = lib.tool("RATLINK", "bin/link")


def missing(output, expected):
    at = 0
    for want in expected:
        at = output.find(want, at)
        if at < 0:
            return want
        at += len(want)
    return None


def flags_of(src):
    first = src.read_text().split("\n", 1)[0]
    return first.removeprefix("// flags:").split() if first.startswith("// flags:") else ["-g"]


def build(name):
    src = s.here / f"{name}.c"
    s.flags = flags_of(src)
    s.compile(name, [src])
    objs = [s.dir / name / f"{name}.o"]
    exe = s.dir / name / name
    result = subprocess.run([str(ratlink), *map(str, objs), "-o", str(exe)], capture_output=True, text=True)
    s.log(result.stdout + result.stderr)
    if result.returncode != 0:
        raise lib.BuildError
    return exe


def check(name):
    try:
        exe = build(name)
    except lib.BuildError:
        return s.fail(name, f"build failed, see {s.dir}/build.log")
    # one -ex per command, so an optimized-out value does not stop the script
    cmds = [arg for line in (s.here / f"{name}.gdb").read_text().splitlines() for arg in ("-ex", line)]
    result = subprocess.run([gdb, "-nx", "-batch", *cmds, str(exe)], capture_output=True, text=True)
    log = s.dir / name / "gdb.log"
    log.write_text(result.stdout + result.stderr)
    want = missing(result.stdout + result.stderr, (s.here / f"{name}.{s.level}.expect").read_text().splitlines())
    if want is None:
        s.ok(name)
    else:
        s.fail(name, f"missing '{want}', see {log}")


def suite():
    if not gdb:
        print(f"SKIP  debug/{s.level}/gdb: gdb not found", flush=True)
        return
    for script in sorted(s.here.glob("*.gdb")):
        check(script.stem)


s.for_levels(suite)
