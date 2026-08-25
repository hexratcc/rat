import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASE = ["-D_DEFAULT_SOURCE"]
BENCH_NAME_W = 36
BENCH_VAL_W = 10


class BuildError(Exception):
    pass


def parse_args(argv):
    jobs, quiet, bench, rest = os.cpu_count(), False, False, []
    args = iter(argv)
    for arg in args:
        if arg == "-q":
            quiet = True
        elif arg == "bench":
            bench = True
        elif arg == "-j":
            jobs = int(next(args))
        elif arg.startswith("-j"):
            jobs = int(arg[2:])
        else:
            rest.append(arg)
    return jobs, quiet, bench, rest


def host_compilers():
    return [cc for cc in ("gcc", "clang") if shutil.which(cc)]


def tool(env, default):
    path = Path(os.environ.get(env, default))
    if not os.access(path, os.X_OK):
        sys.exit(f"missing {path}, run make")
    return path


def bench_header():
    print(" " * 6 + "seconds".ljust(BENCH_NAME_W) + "".join(f"{cc:>{BENCH_VAL_W}}" for cc in ("rat", "gcc", "clang")), flush=True)


def bench_total(totals):
    row = " " * 6 + "total".ljust(BENCH_NAME_W)
    for cc in ("rat", "gcc", "clang"):
        row += f"{round(totals[cc], 3):>{BENCH_VAL_W}g}" if cc in totals else f"{'-':>{BENCH_VAL_W}}"
    print(row, flush=True)


class Suite:
    def __init__(self, name):
        os.chdir(ROOT)  # every path below is repo-relative
        self.name = name
        self.here = Path("src/compiler/test") / name
        self.out = Path("build/test") / name
        self.jobs, self.quiet, self.bench_mode, self.args = parse_args(sys.argv[1:])
        self.ratcc = tool("RATCC", "bin/cc")
        self.hostcc = os.environ.get("HOSTCC", "cc")
        self.passed = 0
        self.benches = 0
        self.totals = {}
        self.failures = []
        self.level = ""
        self.cc = "rat"
        self.dir = Path()
        self.flags = []
        self.ldlibs = []

    def require_checkout(self):
        sub = self.here / self.name
        if not sub.is_dir() or not any(sub.iterdir()):
            sys.exit(f"{self.name} is not checked out, run: git submodule update --init")

    def ok(self, test):
        print(f"PASS  {self.name}/{self.level}/{test}", flush=True)
        self.passed += 1

    def fail(self, test, reason):
        print(f"FAIL  {self.name}/{self.level}/{test}: {reason}", flush=True)
        self.failures.append(f"{self.name}/{self.level}/{test}")

    def use(self, cc):
        self.cc = cc
        self.dir = self.out / self.level / cc
        shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True)
        self.flags = []
        self.ldlibs = []

    def compare(self, results):
        for test, vals in results.items():
            row = "BENCH " + f"{self.name}/{self.level}/{test}".ljust(BENCH_NAME_W)
            for cc in ("rat", "gcc", "clang"):
                row += f"{vals[cc]:>{BENCH_VAL_W}g}" if cc in vals else f"{'-':>{BENCH_VAL_W}}"
                self.totals[cc] = self.totals.get(cc, 0) + vals.get(cc, 0)
            print(row, flush=True)
            self.benches += 1

    def log(self, text):
        if text:
            self.dir.mkdir(parents=True, exist_ok=True)  # survive a concurrent run's rm
            with open(self.dir / "build.log", "a") as f:
                f.write(text)

    def compile(self, sub, srcs):
        out = self.dir / sub
        out.mkdir(parents=True, exist_ok=True)

        def one(src):
            obj = out / (src.stem + ".o")
            if self.cc == "rat":
                cmd = [self.ratcc, f"-{self.level}", *BASE, *self.flags, "-o", obj, src]
            else:  # a host compiler for the bench comparison, warnings are its problem
                cmd = [self.cc, f"-{self.level}", "-w", "-c", *BASE, *self.flags, "-o", obj, src]
            return subprocess.run([str(part) for part in cmd], capture_output=True, text=True)

        with ThreadPoolExecutor(self.jobs) as pool:
            results = list(pool.map(one, sorted(srcs)))
        for result in results:
            self.log(result.stdout + result.stderr)
        if any(result.returncode != 0 for result in results):
            raise BuildError

    def link(self, name, objs, into=None):
        exe = into or self.dir / name
        cmd = [self.hostcc, "-no-pie", "-o", exe, *objs, *self.ldlibs]
        result = subprocess.run([str(part) for part in cmd], capture_output=True, text=True)
        self.log(result.stdout + result.stderr)
        if result.returncode != 0:
            raise BuildError
        return exe

    def build_test(self, src):
        self.compile(".", [src])
        return self.link(src.stem, [self.dir / (src.stem + ".o"), *sorted((self.dir / "lib").glob("*.o"))])

    def run(self, test, cmd, stdin=None):
        with open(self.dir / f"{test}.log", "wb") as log:
            result = subprocess.run([str(part) for part in cmd], stdin=stdin, stdout=log, stderr=log)
        if result.returncode == 0:
            self.ok(test)
        else:
            self.fail(test, f"see {self.dir}/{test}.log")

    def for_levels(self, suite):
        for self.level in ("O1",) if self.bench_mode else ("O0", "O1"):
            self.dir = self.out / self.level
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir.mkdir(parents=True)
            self.flags = []
            self.ldlibs = []
            try:
                suite()
            except BuildError:
                self.fail("build", f"see {self.dir}/build.log")
        if not self.quiet:
            if self.failures:
                print("\n=== failures ===")
                for failure in self.failures:
                    print(f"FAIL  {failure}")
            if self.bench_mode:
                if self.benches:
                    bench_total(self.totals)
                print(f"\n{self.benches} benchmarks, {len(self.failures)} failed")
            else:
                print(f"\n{self.passed} passed, {len(self.failures)} failed")
        sys.exit(1 if self.failures else 0)
