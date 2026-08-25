#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("zlib")
s.require_checkout()
if s.bench_mode:
    sys.exit(0)  # no benchmarks here
zlib = s.here / "zlib"


def suite():
    s.flags = [f"-I{zlib}"]
    s.compile("lib", zlib.glob("*.c"))
    for test in ("example", "minigzip", "infcover"):
        s.build_test(zlib / "test" / f"{test}.c")
    s.run("example", [s.dir / "example", s.dir / "tmp.gz"])
    s.run("infcover", [s.dir / "infcover"])
    data = b"hello world\n"
    packed = subprocess.run([s.dir / "minigzip"], input=data, capture_output=True)
    unpacked = subprocess.run([s.dir / "minigzip", "-d"], input=packed.stdout, capture_output=True)
    if packed.returncode == 0 and unpacked.returncode == 0 and unpacked.stdout == data:
        s.ok("minigzip")
    else:
        s.fail("minigzip", "roundtrip did not reproduce the input")


s.for_levels(suite)
