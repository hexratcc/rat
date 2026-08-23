#!/usr/bin/env bash
# usage: ./build.sh [release|debug]
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
MODE="${1:-release}"
case "$MODE" in
	release) OPT="-O2" ;;
	debug)   OPT="-O0 -g" ;;
	*) echo "usage: $0 [release|debug]" >&2; exit 1 ;;
esac

FLAGS="-std=c++17 -Wall -Wextra -pthread $OPT"

OBJ="build/$MODE"
mkdir -p "$OBJ" bin

# iquote so base string.h cannot shadow the libc header for <...> includes
INC="-iquote src/base -iquote src/backend -iquote src/linker -iquote src/compiler -iquote $OBJ"

# regenerate git_hash.h only when the string changes
git_hash=$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
if ! git diff --quiet HEAD -- 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
	git_hash="${git_hash}-dirty"
fi
hash_hdr="$OBJ/git_hash.h"
new_hdr="#define GIT_HASH \"$git_hash\""
if [ ! -f "$hash_hdr" ] || [ "$(cat "$hash_hdr")" != "$new_hdr" ]; then
	echo "$new_hdr" > "$hash_hdr"
fi

# source groups
base_srcs="src/base/test_harness.cpp"
rat_srcs=$(find src/backend/analysis src/backend/codegen src/backend/ir src/backend/pass src/backend/target -name '*.cpp' | sort)
link_srcs="src/linker/linker.cpp src/linker/elf_write.cpp src/linker/elf_read.cpp"
cc_srcs=$(find src/compiler/emit src/compiler/lex src/compiler/parse -name '*.cpp' | sort; find src/compiler -maxdepth 1 -name '*.cpp' ! -name main.cpp | sort)
driver_srcs="src/backend/main.cpp src/backend/test/runner.cpp \
	src/linker/main.cpp src/compiler/main.cpp src/compiler/test/runner.cpp"

all_srcs="$base_srcs $rat_srcs $link_srcs $cc_srcs $driver_srcs"

obj() { echo "$OBJ/${1//\//_}.o"; }

# stale if the obj is missing or any prereq in its depfile is newer
stale() {
	local out="$1" dep="${1%.o}.d" p
	[ -f "$out" ] && [ -f "$dep" ] || return 0
	while read -r p; do
		[ -n "$p" ] && [ -f "$p" ] && [ "$p" -nt "$out" ] && return 0
	done < <(sed -e 's/^[^:]*://' -e 's/\\$//' "$dep" | tr ' ' '\n')
	return 1
}

# incremental comp
compile() {
	local src="$1" out
	out="$OBJ/${src//\//_}.o"
	stale "$out" || return 0
	echo "cc   $src"
	"$CXX" $FLAGS $INC -MMD -MP -c "$src" -o "$out"
}

export -f compile stale
export CXX FLAGS INC OBJ
echo "$all_srcs" | tr ' ' '\n' | grep . | xargs -P"$(nproc)" -I{} bash -c 'compile "$@"' _ {}

objs() { for s in $1; do obj "$s"; done; }
base_o=$(obj "$base_srcs")
rat_o=$(objs "$rat_srcs")
link_o=$(objs "$link_srcs")
cc_o=$(objs "$cc_srcs")

# relink only when an input object is newer than the binary
link_bin() {
	local out="bin/$1" o; shift
	if [ -f "$out" ]; then
		for o in "$@"; do [ "$o" -nt "$out" ] && break; done
		[ "$o" -nt "$out" ] || return 0
	fi
	echo "link $out"
	"$CXX" $FLAGS -o "$out" "$@"
}

link_bin rat      $base_o $rat_o $(obj src/backend/main.cpp)
link_bin rat-test $base_o $rat_o $(obj src/backend/test/runner.cpp)
link_bin link     $base_o $link_o $(obj src/linker/main.cpp)
link_bin cc       $base_o $rat_o $cc_o $(obj src/compiler/main.cpp)
link_bin cc-test  $base_o $rat_o $cc_o $link_o $(obj src/compiler/test/runner.cpp)
