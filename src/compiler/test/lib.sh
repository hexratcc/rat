# Shared by the per-directory test runners. A runner sets NAME, sources this,
# defines a suite function, and hands it to for_levels. Every path below is
# relative to the repo root, which this file cds to.

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$ROOT"

TEST=src/compiler/test
here=$TEST/$NAME
OUT=build/test/$NAME
RATCC=${RATCC:-bin/cc}
RATLINK=${RATLINK:-bin/link}
HOSTCC=${HOSTCC:-cc}
# -std=c99 hides ftello/strdup in glibc, the projects need them declared
BASE=-D_DEFAULT_SOURCE

[ -x "$RATCC" ] && [ -x "$RATLINK" ] ||
	{ echo "missing $RATCC / $RATLINK, run ./build.sh" >&2; exit 1; }

jobs=$(nproc)
quiet= # -q: only the per-test lines, the caller tallies them
args=()
while [ $# -gt 0 ]; do
	case $1 in
		-j) jobs=$2; shift 2 ;;
		-j*) jobs=${1#-j}; shift ;;
		-q) quiet=1; shift ;;
		*) args+=("$1"); shift ;;
	esac
done

rc=0
level=
dir=
flags=
ldlibs=
passed=0
failures=()

# a test's identity, printed the way the correctness runner prints a case path
id() { echo "$NAME/$level/$1"; }

pass() {
	echo "PASS  $(id "$1")"
	passed=$((passed + 1))
}

fail() {
	echo "FAIL  $(id "$1"): $2"
	failures+=("$(id "$1")")
	rc=1
}

# run one test binary, the exit status is the verdict
run() {
	local name=$1
	shift
	if "$@" > "$dir/$name.log" 2>&1; then
		pass "$name"
	else
		fail "$name" "see $dir/$name.log"
	fi
}

# run the suite once per optimization level, each in a fresh output directory
for_levels() {
	for level in O0 O1; do
		dir=$OUT/$level
		rm -rf "$dir"
		mkdir -p "$dir"
		flags=$BASE
		ldlibs=
		"$1" || fail build "see $dir/build.log"
	done
	[ -z "$quiet" ] || return 0
	if [ ${#failures[@]} -gt 0 ]; then
		echo
		echo "=== failures ==="
		printf 'FAIL  %s\n' "${failures[@]}"
	fi
	echo
	echo "$passed passed, ${#failures[@]} failed"
}

# compile sources into $dir/$1, each object keeps its source name. Build output goes
# to a log so a project's warnings stay out of the way until something actually fails.
compile() {
	local sub=$1 s
	shift
	mkdir -p "$dir/$sub"
	for s in "$@"; do
		$RATCC "-$level" $flags -o "$dir/$sub/$(basename "$s" .c).o" "$s" >> "$dir/build.log" 2>&1 ||
			return 1
	done
}

# ratcc emits objects only, the host cc links them
link() {
	local name=$1
	shift
	$HOSTCC -no-pie -o "$dir/$name" "$@" $ldlibs >> "$dir/build.log" 2>&1 || return 1
}

# compile one test source and link it against the library objects in $dir/lib
build_test() {
	local name
	name=$(basename "$1" .c)
	compile . "$1" || return 1
	link "$name" "$dir/$name.o" "$dir"/lib/*.o || return 1
}

# a vendored project is a submodule, an empty directory means it is not checked out
require_checkout() {
	[ -n "$(ls -A "$here/$NAME" 2> /dev/null)" ] ||
		{ echo "$NAME is not checked out, run: git submodule update --init" >&2; exit 1; }
}
