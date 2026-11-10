#!/usr/bin/env bash
# -g must not change -O1 code. used as RATCC it compiles as asked, then again with -g, and logs every
# object whose .text bytes differ
#   RATCC=src/compiler/test/debug/text_check.sh ./src/compiler/test/run.py -j3
#   then build/test/text_check.log lists the differing objects, one per line
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
cc="$root/bin/cc"
log="$root/build/test/text_check.log"
"$cc" "$@" || exit
obj=""
opt=""
args=()
prev=""
for arg in "$@"; do
	[ "$arg" = "-g" ] && exit 0
	[ "$arg" = "-O1" ] && opt=1
	if [ "$prev" = "-o" ]; then
		obj="$arg"
		arg="$arg.g"
	fi
	args+=("$arg")
	prev="$arg"
done
[ -n "$opt" ] && [ -n "$obj" ] || exit 0
"$cc" -g "${args[@]}" || { echo "$obj: -g build failed" >>"$log"; exit 0; }
objcopy -O binary --only-section=.text "$obj" "$obj.text"
objcopy -O binary --only-section=.text "$obj.g" "$obj.g.text"
cmp -s "$obj.text" "$obj.g.text" || echo "$obj" >>"$log"
rm -f "$obj.g" "$obj.text" "$obj.g.text"
