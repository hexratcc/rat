#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("libyaml")
s.require_checkout()
if s.bench_mode:
    sys.exit(0)  # no benchmarks here
yaml = s.here / "libyaml"


def suite():
    s.flags = [f"-I{yaml}/include", "-DHAVE_CONFIG_H", f"-I{s.here}"]
    s.compile("lib", (yaml / "src").glob("*.c"))
    for test in ("test-version", "test-reader", "test-nesting"):
        s.build_test(yaml / "tests" / f"{test}.c")
        s.run(test, [s.dir / test])
    examples = sorted((yaml / "examples").glob("*.yaml"))
    for test in ("run-scanner", "run-parser", "run-loader", "run-emitter", "run-dumper"):
        s.build_test(yaml / "tests" / f"{test}.c")
        s.run(test, [s.dir / test, *examples])


s.for_levels(suite)
