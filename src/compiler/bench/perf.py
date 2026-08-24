#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FormatStrFormatter

ROOT = Path(__file__).resolve().parents[3]
CACHE = ROOT / "assets/perf.tsv"
PNG = ROOT / "assets/perf.png"
CC_CACHE = ROOT / "assets/compile.tsv"
CC_PNG = ROOT / "assets/compile.png"
WORK = ROOT / "build/perf"
WT = WORK / "wt"
LOGS = WORK / "logs"
BENCH = "src/compiler/bench/bench.c"
THIRD_PARTY = ROOT / "src/compiler/test"

def resolve(name):
    hit = shutil.which(name)
    if hit:
        return hit
    for d in ("/run/current-system/sw/bin", "/usr/local/bin", "/usr/bin"):
        cand = Path(d) / name
        if os.access(cand, os.X_OK):
            return str(cand)
    return name


GCC = resolve(os.environ.get("PERF_GCC", "gcc"))
CLANG = resolve(os.environ.get("PERF_CLANG", "clang"))
HOSTCC = os.environ.get("HOSTCC", "cc")
RUNS = 10
CC_RUNS = 5
BUILD_PIN = ["taskset", "-c", os.environ["PERF_CPUS"]] if os.environ.get("PERF_CPUS") else []
RUN_PIN = ["taskset", "-c", os.environ["PERF_RUN_CPU"]] if os.environ.get("PERF_RUN_CPU") else []

# project dir, source globs, flags
CORPUS = (
    ("sqlite", ("sqlite3.c",),
     ("-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OS_OTHER=1",
      "-DSQLITE_DISABLE_INTRINSIC")),
    ("lz4", ("lib/*.c",), ("-Ilib",)),
    ("wren", ("src/vm/*.c", "src/optional/*.c"),
     ("-Isrc/include", "-Isrc/vm", "-Isrc/optional", "-DWREN_COMPUTED_GOTO=0")),
    ("libyaml", ("src/*.c",), ("-Iinclude", "-DHAVE_CONFIG_H", "-I..")),
    ("zlib", ("[!g]*.c",), ("-I.",)),  # gz*.c wants posix fcntl
)
# -std=c99 hides ftello/strdup in glibc, the corpus needs them declared
SHARED_FLAGS = ("-D_DEFAULT_SOURCE",)
LEVELS = ("-O0", "-O1")
HOST_BASE = ("-std=c99", "-c")

WIDTH, HEIGHT, DPI = 1800, 400, 100
LEFT, RIGHT, TOP = 0.075, 0.997, 0.87
RAT, GCC_COLOR, CLANG_COLOR, INK, TEXT = "#d9532c", "#3d7ee0", "#2ca05a", "#8a9096", "#000000"
PX_PER_PT = DPI / 72


def die(message):
    sys.exit(f"perf: {message}")


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, check=True, capture_output=True, text=True
    ).stdout.strip()


def step(cmd, log, cwd=ROOT):
    """run one build step, a tool the host does not have is a failure and not a crash"""
    try:
        with open(log, "ab") as out:
            return subprocess.run(cmd, cwd=cwd, stdout=out, stderr=out).returncode == 0
    except OSError as e:
        with open(log, "ab") as out:
            out.write(f"{cmd[0]}: {e}\n".encode())
        return False


def build_jobs():
    """cores the builds may use, taskset narrows it when PERF_CPUS is set"""
    if BUILD_PIN:
        try:
            return subprocess.run(
                [*BUILD_PIN, "nproc"], check=True, capture_output=True, text=True
            ).stdout.strip()
        except OSError:
            pass
    return str(len(os.sched_getaffinity(0)))


def build_worktree(log):
    """each segment of the repo had its own build system, all of them drop the compiler in bin/"""
    if (WT / "build.sh").exists():
        steps = [[*BUILD_PIN, "./build.sh", "release"]]
    elif (WT / "CMakeLists.txt").exists():
        steps = [
            [*BUILD_PIN, "cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"],
            [*BUILD_PIN, "cmake", "--build", "build", f"-j{build_jobs()}"],
        ]
    elif (WT / "Makefile").exists():
        steps = [[*BUILD_PIN, "make", f"-j{build_jobs()}"]]
    else:
        return False
    return all(step(s, log, cwd=WT) for s in steps)


def time_binary(binary, log):
    """best wall time over a few runs"""
    best = None
    for _ in range(RUNS):
        try:
            with open(log, "wb") as out:
                start = time.perf_counter_ns()
                code = subprocess.run([*RUN_PIN, binary, "1"], stdout=out, stderr=out).returncode
                elapsed = (time.perf_counter_ns() - start) // 1_000_000
        except OSError:
            return None
        if code != 0 or b"MISMATCH" in Path(log).read_bytes():
            return None
        best = elapsed if best is None else min(best, elapsed)
    return best or None


def log_tail(log):
    if not log.exists():
        return "no output"
    lines = [l.strip() for l in log.read_text(errors="replace").splitlines() if l.strip()]
    if not lines:
        return "no output"
    errors = [l for l in lines if "error:" in l]
    return (errors[0] if errors else lines[-1])[:140]


def build_commit(commit, out, log):
    """build this commit's compiler, None if that is not possible"""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    git("-C", str(WT), "checkout", "-q", "--detach", commit)
    git("-C", str(WT), "clean", "-xdfq")
    if not build_worktree(log):
        return None

    cc = next((WT / c for c in ("bin/cc", "bin/ratcc") if os.access(WT / c, os.X_OK)), None)
    if cc is None:
        log.write_text(f"no compiler binary in {WT}/bin\n")
        return None
    return cc


def measure_runtime(cc, out, log):
    compile = [str(cc), "-O1", "-o", str(out / "rat.o"), str(WORK / "bench.c")]
    link = [HOSTCC, "-no-pie", str(out / "rat.o"), "-o", str(out / "rat"), "-lm"]
    if not all(step(s, log) for s in (compile, link)):
        return None
    return time_binary(str(out / "rat"), out / "rat.run")


def measure_gcc(out):
    out.mkdir(parents=True, exist_ok=True)
    build = [GCC, "-std=c99", "-O1", str(WORK / "bench.c"), "-o", str(out / "gcc"), "-lm"]
    if not step(build, out / "gcc.log"):
        return None
    return time_binary(str(out / "gcc"), out / "gcc.run")


def measure_clang(out):
    out.mkdir(parents=True, exist_ok=True)
    build = [CLANG, "-std=c99", "-O1", str(WORK / "bench.c"), "-o", str(out / "clang"), "-lm"]
    if not step(build, out / "clang.log"):
        return None
    return time_binary(str(out / "clang"), out / "clang.run")


def corpus_jobs():
    jobs = []
    for name, globs, flags in CORPUS:
        d = THIRD_PARTY / name / name  # test/<name>/ holds the harness, the submodule sits inside
        srcs = sorted(str(p.relative_to(d)) for g in globs for p in d.glob(g))
        if not srcs:
            die(f"no sources under {d}, run: git submodule update --init")
        jobs.append((d, srcs, flags))
    return jobs


def compile_corpus(jobs, binary, base, out, sink):
    for d, srcs, flags in jobs:
        for i, src in enumerate(srcs):
            obj = str(out / f"{d.name}{i}.o")
            cmd = [*RUN_PIN, binary, *base, *SHARED_FLAGS, *flags, "-o", obj, src]
            try:
                code = subprocess.run(cmd, cwd=d, stdout=sink, stderr=sink).returncode
            except OSError as e:
                sink.write(f"{cmd[0]}: {e}\n".encode())
                return False
            if code != 0:
                sink.write(f"failed: {' '.join(cmd)}\n".encode())
                return False
    return True


def time_compiler(binary, base, out, log):
    jobs = corpus_jobs()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    total = 0
    with open(log, "wb") as sink:
        for run in range(CC_RUNS + 1):  # run 0 is a warmup, it pays the page cache misses
            start = time.perf_counter_ns()
            ok = compile_corpus(jobs, binary, base, out, sink)
            elapsed = time.perf_counter_ns() - start
            if not ok:
                return None
            if run:
                total += elapsed
    return total // (CC_RUNS * 1_000_000)


def load_cache(path):
    if not path.exists():
        # the graphs and their caches
        try:
            path.write_text(git("show", f"origin/perf:{path.name}") + "\n")
        except subprocess.CalledProcessError:
            path.write_text("# kind\tcommit\tbench\tmeasured\tms\n")
            return {}
    rows = (l.split("\t") for l in path.read_text().splitlines() if not l.startswith("#"))
    return {(r[0], r[1], r[2]): int(r[4]) for r in rows if len(r) == 5}


def record(path, cache, kind, commit, bench, ms):
    stamp = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    with open(path, "a") as f:
        f.write(f"{kind}\t{commit}\t{bench}\t{stamp}\t{ms}\n")
    cache[(kind, commit, bench)] = ms


def carry(values):
    out = list(values)
    nxt = None
    for i in reversed(range(len(out))):
        nxt = out[i] or nxt
        out[i] = nxt
    prev = None
    for i in range(len(out)):
        prev = out[i] or prev
        out[i] = prev
    return out


def sha_layout(shas):
    pitch = WIDTH * (RIGHT - LEFT) / max(len(shas), 1)
    size = min(16.0, max(3.0, pitch * 0.85 / PX_PER_PT))
    length = max(len(s) for s in shas) * 0.62 * size * PX_PER_PT
    return size, min(0.45, (length + 14) / HEIGHT)


def plot(shas, lines, hlines, out, title, ylabel):
    fig, ax = plt.subplots(figsize=(WIDTH / DPI, HEIGHT / DPI), dpi=DPI)
    x = range(len(shas))

    for times, color, label, dash in lines:
        ax.plot(x, times, color=color, linewidth=2.6, linestyle=dash, label=label)
    for value, color, label, dash in hlines:
        ax.axhline(value, color=color, linewidth=2.6, linestyle=dash, label=label)

    # pad the range
    values = [v for times, _, _, _ in lines for v in times] + [h[0] for h in hlines]
    lo, hi = min(values), max(values)
    span = (hi - lo) or hi * 0.1
    ax.set_ylim(lo - span * 0.15, hi + span * 0.15)
    ax.set_xlim(-0.5, len(shas) - 0.5)

    size, bottom = sha_layout(shas)
    ax.set_xticks(list(x))
    ax.set_xticklabels(shas, rotation=90, fontsize=size, color=TEXT, fontfamily="monospace")
    ax.tick_params(axis="x", length=2, pad=1.5, color=TEXT)
    ax.tick_params(axis="y", labelsize=17, length=3, pad=2, color=TEXT, labelcolor=TEXT)
    ax.yaxis.set_major_formatter(FormatStrFormatter("%.2f"))
    ax.set_ylabel(ylabel, fontsize=18, color=TEXT, labelpad=6)

    ax.grid(axis="y", color=INK, linewidth=0.5, linestyle=(0, (2, 3)), alpha=0.6)
    ax.set_axisbelow(True)
    for side, spine in ax.spines.items():
        spine.set_visible(side in ("left", "bottom"))
        spine.set_color(TEXT)
        spine.set_linewidth(1.2)

    ax.set_title(title, fontsize=26, color=TEXT, pad=44)
    ax.legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.005),
        fontsize=21,
        frameon=False,
        labelcolor=TEXT,
        handlelength=1.6,
        borderpad=0.2,
        ncol=len(lines) + len(hlines),
        columnspacing=1.6,
    )

    fig.subplots_adjust(left=LEFT, right=RIGHT, top=TOP - 0.09, bottom=bottom)
    fig.savefig(out, dpi=DPI)


def runtime_refs(cache, bench, out):
    if ("gcc", "-", bench) not in cache:
        ms = measure_gcc(out)
        if ms is None:
            die(f"gcc reference run failed, see {out / 'gcc.log'}")
        print(f"gcc -O1 reference: {ms} ms")
        record(CACHE, cache, "gcc", "-", bench, ms)
    if ("clang", "-", bench) not in cache:
        ms = measure_clang(out)
        if ms is not None:
            print(f"clang -O1 reference: {ms} ms")
            record(CACHE, cache, "clang", "-", bench, ms)
        else:
            # report the stage that failed while its log still exists (the commit loop wipes out/)
            blog, rlog = out / "clang.log", out / "clang.run"
            stage = (f"build: {log_tail(blog)}" if blog.exists() and blog.stat().st_size
                     else f"run: {log_tail(rlog)}")
            print(f"clang -O1 reference: unavailable ({stage})")


def compile_refs(cache, corpus, out):
    """one reference sample per host compiler and optimization level"""
    for tool, binary in (("gcc", GCC), ("clang", CLANG)):
        for level in LEVELS:
            kind = tool + level
            if (kind, "-", corpus) in cache:
                continue
            log = LOGS / f"{kind}.cc.log"
            ms = time_compiler(binary, HOST_BASE + (level,), out, log)
            if ms is None:
                if tool == "gcc":
                    die(f"gcc {level} corpus build failed, see {log}")
                print(f"clang {level} corpus: unavailable ({log_tail(log)})")
                continue
            print(f"{tool} {level} corpus: {ms} ms")
            record(CC_CACHE, cache, kind, "-", corpus, ms)


def draw_runtime(cache, commits, bench):
    measured = [cache[("rat", c, bench)] for c in commits]
    if not any(measured):
        die("no usable runtime measurements")
    lines = [([ms / 1000.0 for ms in carry(measured)], RAT, "rat -O1", "-")]
    hlines = [(cache[("gcc", "-", bench)] / 1000.0, GCC_COLOR, "gcc -O1", "-")]
    clang_ref = cache.get(("clang", "-", bench))
    if clang_ref:
        hlines.append((clang_ref / 1000.0, CLANG_COLOR, "clang -O1", "-"))
    plot([c[:7] for c in commits], lines, hlines, PNG,
         "rat vs gcc/clang bench.c", "runtime (sec)")
    print(f"perf: wrote {PNG.relative_to(ROOT)} "
          f"({len(commits)} commits, {sum(1 for m in measured if m)} measured)")


def draw_compile(cache, commits, corpus):
    dashes = (("-O1", "-"), ("-O0", (0, (5, 2))))
    lines, hlines, seen = [], [], 0
    for level, dash in dashes:
        measured = [cache.get(("rat" + level, c, corpus), 0) for c in commits]
        if not any(measured):
            die(f"no usable rat {level} corpus measurements")
        seen = max(seen, sum(1 for m in measured if m))
        lines.append(([ms / 1000.0 for ms in carry(measured)], RAT, f"rat {level}", dash))
    for tool, color in (("gcc", GCC_COLOR), ("clang", CLANG_COLOR)):
        for level, dash in dashes:
            ms = cache.get((tool + level, "-", corpus))
            if ms:
                hlines.append((ms / 1000.0, color, f"{tool} {level}", dash))
    plot([c[:7] for c in commits], lines, hlines, CC_PNG,
         "rat vs gcc/clang compile speed", "corpus build (sec)")
    print(f"perf: wrote {CC_PNG.relative_to(ROOT)} ({len(commits)} commits, {seen} measured)")


def main(argv):
    count = int(argv[1]) if len(argv) > 1 else 100

    bench = git("log", "-1", "--format=%H", "HEAD", "--", BENCH)
    if not bench:
        die(f"{BENCH} is not tracked")
    projects = [str(THIRD_PARTY / name / name) for name, _, _ in CORPUS]
    corpus = git("log", "-1", "--format=%H", "HEAD", "--", *projects)
    if not corpus:
        die(f"{THIRD_PARTY} has no projects, run: git submodule update --init")

    LOGS.mkdir(parents=True, exist_ok=True)
    (WORK / "bench.c").write_text(git("show", f"HEAD:{BENCH}"))

    git("worktree", "prune")
    if not WT.exists():
        git("worktree", "add", "-q", "--detach", str(WT), "HEAD")

    cache, ccache = load_cache(CACHE), load_cache(CC_CACHE)
    commits = git("rev-list", "--first-parent", "-n", str(count), "HEAD").split()[::-1]
    print(f"perf: {len(commits)} commits, bench from {bench[:12]}, corpus from {corpus[:12]}")

    out, ccout = WORK / "out", WORK / "ccout"
    runtime_refs(cache, bench, out)
    compile_refs(ccache, corpus, ccout)

    for commit in commits:
        want_rt = ("rat", commit, bench) not in cache
        levels = [l for l in LEVELS if ("rat" + l, commit, corpus) not in ccache]
        if not (want_rt or levels):
            continue
        date = git("log", "-1", "--format=%cs", commit)
        print(f"{commit[:12]} {date}  ", end="", flush=True)
        log = LOGS / f"{commit[:12]}.log"
        cc = build_commit(commit, out, log)
        if want_rt:
            ms = measure_runtime(cc, out, log) if cc else None
            print(f"rat {ms:6} ms  " if ms else f"failed: {log_tail(log)}  ", end="", flush=True)
            record(CACHE, cache, "rat", commit, bench, ms or 0)
        for level in levels:
            cclog = LOGS / f"{commit[:12]}{level}.cc.log"
            ms = time_compiler(str(cc), (level,), ccout, cclog) if cc else None
            print(f"{level} {ms:6} ms  " if ms else f"{level} failed  ", end="", flush=True)
            record(CC_CACHE, ccache, "rat" + level, commit, corpus, ms or 0)
        print(flush=True)

    draw_runtime(cache, commits, bench)
    draw_compile(ccache, commits, corpus)


if __name__ == "__main__":
    main(sys.argv)
