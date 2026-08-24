#!/usr/bin/env bash
set -euo pipefail
NAME=sqlite
. "$(dirname "$0")/../lib.sh"
require_checkout

suite() {
	local s=$here/sqlite
	flags="$BASE -I$s -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OS_UNIX=1 -DSQLITE_DISABLE_INTRINSIC"
	ldlibs=-lm
	compile . "$s/sqlite3.c" "$s/shell.c" || return 1
	link sqlite3 "$dir/sqlite3.o" "$dir/shell.o" || return 1
	rm -f "$dir/test.db"
	if "$dir/sqlite3" "$dir/test.db" < "$here/test.sql" > "$dir/shell.log" 2>&1 &&
		diff "$here/test.expected" "$dir/shell.log" > "$dir/shell.diff"; then
		pass shell
	else
		fail shell "see $dir/shell.log"
	fi
}

for_levels suite
exit $rc
