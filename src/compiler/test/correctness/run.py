#!/usr/bin/env python3
# usage: run.py [-j N] [-q] [case.c...]
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

CORPUS = ("c99", "c-testsuite", "custom", "gcc-torture")
HOST_OS = "linux"
TIMEOUT = 20

# the wrapper reports what main returned through a file, because an exit status only carries 8 bits 
# bin/link -e makes libc call it instead of main
WRAPPER = """\
#include <stdio.h>
#include <stdlib.h>

extern int main(int, char**);

static char* args[] = {(char*)"a.out", 0};

int __ratcc_entry(void) {
	int r = main(1, args);
	fflush(stdout);
	const char* path = getenv("RATCC_RET");
	FILE* f = path ? fopen(path, "w") : 0;
	if(f) {
		fprintf(f, "%d", r);
		fclose(f);
	}
	return r;
}
"""

jobs, quiet, bench, picked = lib.parse_args(sys.argv[1:])
if bench:
    sys.exit(0)  # no benchmarks here
os.chdir(lib.ROOT)
here = Path("src/compiler/test/correctness")
out = Path("build/test/correctness")
ratcc = lib.tool("RATCC", "bin/cc")
ratlink = lib.tool("RATLINK", "bin/link")


def os_matches(spec):
    return spec == HOST_OS or spec.endswith("-" + HOST_OS)


def to_i32(value):
    return (value + 2**31) % 2**32 - 2**31


def first_line(result):
    lines = (result.stderr + result.stdout).splitlines()
    return lines[0] if lines else f"exit {result.returncode}"


class Directives:
    def __init__(self, case):
        self.value = None
        self.passes = None  # None means -O1, "" means -O0, else -fpasses=
        self.output = None
        self.skip = False
        self.incs = []
        pinned = False
        in_output = False
        for line in case.read_text(errors="replace").splitlines():
            body = line.lstrip(" \t")
            if not body.startswith("//"):
                in_output = False
                continue
            body = body[2:]
            if in_output and body.startswith("|"):
                self.output += body[1:].removeprefix(" ") + "\n"
                continue
            in_output = False
            key, sep, val = body.partition(":")
            key, val = key.strip(), val.strip()
            if not sep:
                continue
            if key == "expect" and not pinned:
                self.value = to_i32(int(val, 0))
            elif key.startswith("expect-") and os_matches(key[7:]):
                self.value = to_i32(int(val, 0))
                pinned = True
            elif key == "passes":
                self.passes = val
            elif key == "output":
                self.output = ""
                in_output = True
            elif key in ("skip-target", "skip-x86-target") and os_matches(val):
                self.skip = True
            elif key == "include-dir":
                self.incs.append(val)

    def cc_flags(self, case):
        if self.passes is None:
            opt = ["-O1"]
        elif self.passes:
            opt = ["-O0", f"-fpasses={self.passes}"]
        else:
            opt = ["-O0"]
        return opt + [f"-I{case.parent / inc}" for inc in self.incs]


def run_case(case):
    case = Path(case)
    work = out / "cases" / str(case).replace("/", "_")
    work.mkdir(parents=True, exist_ok=True)

    def fail(reason):
        print(f"FAIL  {case}: {reason}", flush=True)
        return str(case)

    exp = Directives(case)
    if exp.value is None:
        return fail("missing '// expect:' directive")
    if exp.skip:  # not for this target
        print(f"PASS  {case}", flush=True)
        return None

    compiled = subprocess.run([str(ratcc), *exp.cc_flags(case), "-o", str(work / "case.o"), str(case)], capture_output=True, text=True)
    if compiled.returncode != 0:
        return fail(first_line(compiled))
    linked = subprocess.run([str(ratlink), "-e", "__ratcc_entry", str(out / "wrap.o"), str(work / "case.o"), "-o", str(work / "case")], capture_output=True, text=True)
    if linked.returncode != 0:
        return fail(f"link failed: {first_line(linked)}")

    ret = work / "ret"
    ret.unlink(missing_ok=True)
    try:
        ran = subprocess.run([str(work / "case")], env={**os.environ, "RATCC_RET": str(ret)}, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return fail(f"timed out after {TIMEOUT}s")
    if ret.is_file() and ret.read_text():
        got = int(ret.read_text())
    else:  # no report means it died early; signals read the way a shell reports them
        got = 128 - ran.returncode if ran.returncode < 0 else ran.returncode

    if got != exp.value:
        return fail(f"expected {exp.value}, got {got}")
    if exp.output is not None and ran.stdout != exp.output.encode():
        return fail("stdout mismatch")
    print(f"PASS  {case}", flush=True)
    return None


def announce_skipped():
    skips = set()
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
    return skips


skips = set() if picked else announce_skipped()
cases = picked or sorted(str(case) for d in CORPUS for case in (here / d).rglob("*.c") if str(case) not in skips)

shutil.rmtree(out, ignore_errors=True)
out.mkdir(parents=True)
(out / "wrap.c").write_text(WRAPPER)
wrap = subprocess.run([str(ratcc), "-O1", "-o", str(out / "wrap.o"), str(out / "wrap.c")], capture_output=True, text=True)
if wrap.returncode != 0:
    sys.exit(f"wrapper compile failed: {first_line(wrap)}")

with ThreadPoolExecutor(jobs) as pool:
    failures = sorted(failure for failure in pool.map(run_case, cases) if failure)
if not quiet:
    if failures:
        print("\n=== failures ===")
        for failure in failures:
            print(f"FAIL  {failure}")
    print(f"\n{len(cases) - len(failures)} passed, {len(failures)} failed")
    print(f"{len(skips)} skipped")
sys.exit(1 if failures else 0)
