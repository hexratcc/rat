#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lib

s = lib.Suite("sqlite")
s.require_checkout()
if s.bench_mode:
    sys.exit(0)
sqlite = s.here / "sqlite"


def suite():
    s.flags = [f"-I{sqlite}", "-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OS_UNIX=1", "-DSQLITE_DISABLE_INTRINSIC"]
    s.ldlibs = ["-lm"]
    s.compile(".", [sqlite / "sqlite3.c", sqlite / "shell.c"])
    exe = s.link("sqlite3", [s.dir / "sqlite3.o", s.dir / "shell.o"])
    (s.dir / "test.db").unlink(missing_ok=True)
    with open(s.here / "test.sql") as sql, open(s.dir / "shell.log", "w") as log:
        result = subprocess.run([str(exe), str(s.dir / "test.db")], stdin=sql, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode == 0 and (s.dir / "shell.log").read_bytes() == (s.here / "test.expected").read_bytes():
        s.ok("shell")
    else:
        s.fail("shell", f"see {s.dir}/shell.log")


s.for_levels(suite)
