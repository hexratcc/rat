#!/usr/bin/env bash
# usage: ./test.sh [name...]

set -euo pipefail
cd "$(dirname "$0")"

[ -x bin/rat-test ] && [ -x bin/cc ] || { echo "missing binaries, run ./build.sh" >&2; exit 1; }

J="-j$(nproc)"
rc=0

names=("$@")
run_ir=
if [ ${#names[@]} -eq 0 ]; then
	for d in src/compiler/test/*/; do names+=("$(basename "$d")"); done
	run_ir=1
fi

log=build/test/results.log
mkdir -p "$(dirname "$log")"
: > "$log"

report() { "$@" | tee -a "$log" || return "${PIPESTATUS[0]}"; }

[ -z "$run_ir" ] || report bash -c 'cd src/backend && exec ../../bin/rat-test "$@"' _ $J -q || rc=1

for name in "${names[@]}"; do
	runner=src/compiler/test/$name/run.sh
	[ -x "$runner" ] || { echo "no such suite: $name" >&2; exit 1; }
	report "$runner" $J -q || rc=1
done

count() { grep -c "^$1" "$log" || true; }
echo
echo "$(count PASS) passed, $(count FAIL) failed, $(count SKIP) skipped"

exit $rc
