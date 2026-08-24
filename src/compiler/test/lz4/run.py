#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("lz4")
s.require_checkout()
if s.bench_mode:
    sys.exit(0)  # no benchmarks here
lz4 = s.here / "lz4"


def suite():
    s.flags = [f"-I{lz4}/lib", f"-I{lz4}/programs", f"-I{lz4}/tests"]
    s.compile("lib", (lz4 / "lib").glob("*.c"))
    for test in ("fuzzer", "frametest", "roundTripTest", "abiTest", "decompress-partial", "decompress-partial-usingDict"):
        s.build_test(lz4 / "tests" / f"{test}.c")
    s.run("fuzzer", [s.dir / "fuzzer", "-i1"])
    s.run("frametest", [s.dir / "frametest", "-i1"])
    s.run("roundTripTest", [s.dir / "roundTripTest", lz4 / "lib/lz4.c"])
    s.run("abiTest", [s.dir / "abiTest", lz4 / "lib/lz4.c"])
    s.run("decompress-partial", [s.dir / "decompress-partial"])
    s.run("decompress-partial-usingDict", [s.dir / "decompress-partial-usingDict"])


s.for_levels(suite)
