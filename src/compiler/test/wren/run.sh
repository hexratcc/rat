#!/usr/bin/env bash
set -euo pipefail
NAME=wren
. "$(dirname "$0")/../lib.sh"
require_checkout

tally() {
	tr '\r' '\n' < "$dir/suite.log" | sed -e 's/\x1b\[[0-9;]*m//g' -e '/^$/d' | tail -1
}

suite() {
	local w=$here/wren tree=$OUT/tree
	flags="$BASE -I$w/src/include -I$w/src/vm -I$w/src/optional -I$w/test -I$w/test/api -DWREN_COMPUTED_GOTO=0"
	ldlibs=-lm
	compile . "$w"/src/vm/*.c "$w"/src/optional/*.c "$w"/test/*.c "$w"/test/api/*.c || return 1
	if [ ! -d "$tree" ]; then
		mkdir -p "$tree/bin"
		cp -a "$w/util" "$w/test" "$w/example" "$tree"
	fi
	$HOSTCC -no-pie -o "$tree/bin/wren_test_$level" "$dir"/*.o -lm >> "$dir/build.log" 2>&1 || return 1
	if (cd "$tree" && python3 util/test.py --suffix="_$level") > "$dir/suite.log" 2>&1; then
		pass suite
	else
		fail suite "$(tally), see $dir/suite.log"
	fi
}

command -v python3 > /dev/null || { echo "wren needs python3 for util/test.py" >&2; exit 1; }
for_levels suite
exit $rc
