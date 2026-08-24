#!/usr/bin/env bash
# lz4: the C tests under tests/, the fuzzers cut to one round
set -euo pipefail
NAME=lz4
. "$(dirname "$0")/../lib.sh"
require_checkout

suite() {
	local l=$here/lz4 t
	flags="$BASE -I$l/lib -I$l/programs -I$l/tests"
	compile lib "$l"/lib/*.c || return 1
	for t in fuzzer frametest roundTripTest abiTest decompress-partial decompress-partial-usingDict; do
		build_test "$l/tests/$t.c" || return 1
	done
	run fuzzer "$dir/fuzzer" -i1
	run frametest "$dir/frametest" -i1
	run roundTripTest "$dir/roundTripTest" "$l/lib/lz4.c"
	run abiTest "$dir/abiTest" "$l/lib/lz4.c"
	run decompress-partial "$dir/decompress-partial"
	run decompress-partial-usingDict "$dir/decompress-partial-usingDict"
}

for_levels suite
exit $rc
