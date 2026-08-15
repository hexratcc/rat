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
WORK = ROOT / "build/perf"
WT = WORK / "wt"
LOGS = WORK / "logs"
BENCH = "src/compiler/bench/bench.c"

GCC = os.environ.get("PERF_GCC", "gcc")
HOSTCC = os.environ.get("HOSTCC", "cc")
RUNS = 10
BUILD_PIN = ["taskset", "-c", os.environ["PERF_CPUS"]] if os.environ.get("PERF_CPUS") else []
RUN_PIN = ["taskset", "-c", os.environ["PERF_RUN_CPU"]] if os.environ.get("PERF_RUN_CPU") else []

WIDTH, HEIGHT, DPI = 1800, 400, 100
LEFT, RIGHT, TOP = 0.075, 0.997, 0.885
RAT, GCC_COLOR, INK, TEXT = "#d9532c", "#3d7ee0", "#8a9096", "#5f6368"
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


def measure_commit(commit, out, log):
    """build this commit's compiler, run HEAD's bench through it, None if that is not possible"""
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


def load_cache():
    if not CACHE.exists():
        # the graph and its cache live on the perf branch, never on main
        try:
            CACHE.write_text(git("show", "origin/perf:perf.tsv") + "\n")
        except subprocess.CalledProcessError:
            CACHE.write_text("# kind\tcommit\tbench\tmeasured\tms\n")
            return {}
    rows = (l.split("\t") for l in CACHE.read_text().splitlines() if not l.startswith("#"))
    return {(r[0], r[1], r[2]): int(r[4]) for r in rows if len(r) == 5}


def record(cache, kind, commit, bench, ms):
    stamp = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    with open(CACHE, "a") as f:
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


def plot(shas, times, gcc, out):
    fig, ax = plt.subplots(figsize=(WIDTH / DPI, HEIGHT / DPI), dpi=DPI)
    x = range(len(shas))

    ax.plot(x, times, color=RAT, linewidth=2.6, label="rat -O1")
    ax.axhline(gcc, color=GCC_COLOR, linewidth=2.6, label="gcc -O1")

    # pad the range
    lo, hi = min(times + [gcc]), max(times + [gcc])
    span = (hi - lo) or hi * 0.1
    ax.set_ylim(lo - span * 0.15, hi + span * 0.15)
    ax.set_xlim(-0.5, len(shas) - 0.5)

    size, bottom = sha_layout(shas)
    ax.set_xticks(list(x))
    ax.set_xticklabels(shas, rotation=90, fontsize=size, color=TEXT, fontfamily="monospace")
    ax.tick_params(axis="x", length=2, pad=1.5, color=INK)
    ax.tick_params(axis="y", labelsize=17, length=3, pad=2, color=INK, labelcolor=TEXT)
    ax.yaxis.set_major_formatter(FormatStrFormatter("%.2f"))
    ax.set_ylabel("runtime (sec)", fontsize=18, color=TEXT, labelpad=6)
    ax.set_title("rat vs gcc bench.c", fontsize=21, color=TEXT, pad=8)

    ax.grid(axis="y", color=INK, linewidth=0.5, linestyle=(0, (2, 3)), alpha=0.6)
    ax.set_axisbelow(True)
    for side, spine in ax.spines.items():
        spine.set_visible(side in ("left", "bottom"))
        spine.set_color(INK)
        spine.set_linewidth(0.5)

    ax.legend(
        loc="upper right",
        fontsize=17,
        frameon=False,
        labelcolor=TEXT,
        handlelength=1.6,
        borderpad=0.2,
        labelspacing=0.2,
    )

    fig.subplots_adjust(left=LEFT, right=RIGHT, top=TOP, bottom=bottom)
    fig.savefig(out, dpi=DPI)


def main(argv):
    count = int(argv[1]) if len(argv) > 1 else 100

    # bench is always HEAD's
    bench = git("log", "-1", "--format=%H", "HEAD", "--", BENCH)
    if not bench:
        die(f"{BENCH} is not tracked")

    LOGS.mkdir(parents=True, exist_ok=True)
    (WORK / "bench.c").write_text(git("show", f"HEAD:{BENCH}"))

    git("worktree", "prune")
    if not WT.exists():
        git("worktree", "add", "-q", "--detach", str(WT), "HEAD")

    cache = load_cache()
    commits = git("rev-list", "--first-parent", "-n", str(count), "HEAD").split()[::-1]
    print(f"perf: {len(commits)} commits, bench from {bench[:12]}")

    out = WORK / "out"
    # one reference sample
    if ("gcc", "-", bench) not in cache:
        ms = measure_gcc(out)
        if ms is None:
            die(f"gcc reference run failed, see {out / 'gcc.log'}")
        print(f"gcc -O1 reference: {ms} ms")
        record(cache, "gcc", "-", bench, ms)

    for commit in commits:
        if ("rat", commit, bench) in cache:
            continue
        date = git("log", "-1", "--format=%cs", commit)
        print(f"{commit[:12]} {date}  ", end="", flush=True)
        log = LOGS / f"{commit[:12]}.log"
        ms = measure_commit(commit, out, log)
        print(f"rat {ms:6} ms" if ms else f"failed: {log_tail(log)}")
        record(cache, "rat", commit, bench, ms or 0)

    measured = [cache[("rat", c, bench)] for c in commits]
    if not any(measured):
        die("no usable measurements")

    shas = [c[:7] for c in commits]
    times = [ms / 1000.0 for ms in carry(measured)]
    plot(shas, times, cache[("gcc", "-", bench)] / 1000.0, PNG)
    print(f"perf: wrote {PNG.relative_to(ROOT)} "
          f"({len(commits)} commits, {sum(1 for m in measured if m)} measured)")


if __name__ == "__main__":
    main(sys.argv)
