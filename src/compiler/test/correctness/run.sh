#!/usr/bin/env bash
# usage: run.sh [-j N] [case.c...]
set -euo pipefail
NAME=correctness
. "$(dirname "$0")/../lib.sh"

CORPUS="c99 c-testsuite custom gcc-torture"
RUNNER=${RUNNER:-bin/cc-test}
picked=("${args[@]+"${args[@]}"}") # named cases stand in for the whole suite

[ -x "$RUNNER" ] || { echo "missing $RUNNER, run ./build.sh" >&2; exit 1; }

case_list() {
	local d
	if [ ${#picked[@]} -gt 0 ]; then
		printf '%s\n' "${picked[@]}"
		return
	fi
	for d in $CORPUS; do
		find "$here/$d" -name '*.c'
	done | sort
}

# report every case ./skip names, and collect their paths in $1 to filter out
announce_skipped() {
	local p reason
	: > "$1"
	while read -r p reason; do
		case $p in '' | '#'*) continue ;; esac
		if [ -f "$here/$p" ]; then
			echo "$here/$p" >> "$1"
			echo "SKIP  $here/$p: $reason"
		else
			echo "skip: no such case '$p'" >&2
		fi
	done < "$here/skip"
}

mkdir -p "$OUT"
skips=$OUT/skip
: > "$skips"
# naming cases explicitly runs them even when the skip file lists them
[ ${#picked[@]} -gt 0 ] || announce_skipped "$skips"

mapfile -t cases < <(case_list | grep -vxF -f "$skips" || true)
"$RUNNER" "-j$jobs" ${quiet:+-q} "${cases[@]}" || rc=1
[ -n "$quiet" ] || echo "$(wc -l < "$skips") skipped"
exit $rc
