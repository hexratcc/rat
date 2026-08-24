#!/usr/bin/env bash
set -euo pipefail
NAME=zlib
. "$(dirname "$0")/../lib.sh"
require_checkout

suite() {
	local z=$here/zlib t
	flags="$BASE -I$z"
	compile lib "$z"/*.c || return 1
	for t in example minigzip infcover; do
		build_test "$z/test/$t.c" || return 1
	done
	run example "$dir/example" "$dir/tmp.gz"
	run infcover "$dir/infcover"
	if [ "$(echo hello world | "$dir/minigzip" | "$dir/minigzip" -d)" = "hello world" ]; then
		pass minigzip
	else
		fail minigzip "roundtrip did not reproduce the input"
	fi
}

for_levels suite
exit $rc
