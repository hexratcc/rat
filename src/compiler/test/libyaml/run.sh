#!/usr/bin/env bash
# libyaml: TESTS= in tests/Makefile.am, then the runners over the shipped yaml
set -euo pipefail
NAME=libyaml
. "$(dirname "$0")/../lib.sh"
require_checkout

suite() {
	local y=$here/libyaml t
	flags="$BASE -I$y/include -DHAVE_CONFIG_H -I$here"
	compile lib "$y"/src/*.c || return 1
	for t in test-version test-reader test-nesting; do
		build_test "$y/tests/$t.c" || return 1
		run "$t" "$dir/$t"
	done
	for t in run-scanner run-parser run-loader run-emitter run-dumper; do
		build_test "$y/tests/$t.c" || return 1
		run "$t" "$dir/$t" "$y"/examples/*.yaml
	done
}

for_levels suite
exit $rc
