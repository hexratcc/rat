#!/usr/bin/env python3
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("wren")
s.require_checkout()
wren = s.here / "wren"


def build(into):
    s.flags = [f"-I{wren}/src/include", f"-I{wren}/src/vm", f"-I{wren}/src/optional", f"-I{wren}/test", f"-I{wren}/test/api", "-DWREN_COMPUTED_GOTO=0"]
    s.ldlibs = ["-lm"]
    s.compile(".", [*(wren / "src/vm").glob("*.c"), *(wren / "src/optional").glob("*.c"), *(wren / "test").glob("*.c"), *(wren / "test/api").glob("*.c")])
    tree = s.out / "tree"
    if not tree.is_dir():
        (tree / "bin").mkdir(parents=True)
        for part in ("util", "test", "example"):
            shutil.copytree(wren / part, tree / part)
    return tree, s.link("wren_test", sorted(s.dir.glob("*.o")), into=into)


def tally():
    text = (s.dir / "suite.log").read_text(errors="replace").replace("\r", "\n")
    lines = [line for line in re.sub("\x1b\\[[0-9;]*m", "", text).splitlines() if line.strip()]
    return lines[-1] if lines else "no output"


def suite():
    tree, _ = build(s.out / "tree/bin" / f"wren_test_{s.level}")
    with open(s.dir / "suite.log", "wb") as log:
        result = subprocess.run([sys.executable, "util/test.py", f"--suffix=_{s.level}"], cwd=tree, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode == 0:
        s.ok("suite")
    else:
        s.fail("suite", f"{tally()}, see {s.dir}/suite.log")


def bench():
    results = {}
    for cc in ("rat", *lib.host_compilers()):
        s.use(cc)
        try:
            tree, exe = build(None)
        except lib.BuildError:
            s.fail(f"{cc} build", f"see {s.dir}/build.log")
            continue
        for script in sorted((tree / "test/benchmark").glob("*.wren")):
            result = subprocess.run([str(Path(exe).resolve()), str(script.relative_to(tree))], cwd=tree, capture_output=True, text=True)
            s.log(result.stdout + result.stderr)
            elapsed = re.search(r"elapsed: ([\d.]+)", result.stdout)
            if result.returncode == 0 and elapsed:
                results.setdefault(script.stem, {})[cc] = round(float(elapsed.group(1)), 3)
            else:
                s.fail(f"{cc} {script.stem}", f"see {s.dir}/build.log")
    if not s.quiet:
        lib.bench_header()
    s.compare(results)


s.for_levels(bench if s.bench_mode else suite)
