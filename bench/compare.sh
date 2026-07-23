#!/bin/sh
# build bench/bench.c with ratcc (all opt levels) and the local cc (o0..o3),
# run each and print a side-by-side avg-ms-per-scenario table
# usage: compare.sh <ratcc> <cc> <src> <outdir> [reps]
set -eu

ratcc=$1
cc=$2
src=$3
out=$4
reps=${5:-5}

mkdir -p "$out"
labels=""
bins=""
add() { labels="$labels $1"; bins="$bins $2"; }

build() {
	log=$1
	shift
	if ! "$@" 2> "$log"; then
		echo "build failed: $*" >&2
		cat "$log" >&2
		exit 1
	fi
}

# ratcc
for l in 0 1; do
	o="$out/ratcc_O$l"
	build "$o.build.log" "$ratcc" -O$l -o "$o.o" "$src"
	build "$o.link.log" "$cc" -no-pie "$o.o" -o "$o" -lm
	add "ratcc-O$l" "$o"
done

# cc
for l in 0 1 2 3; do
	o="$out/cc_O$l"
	build "$o.build.log" "$cc" -std=c99 -O$l "$src" -o "$o" -lm
	add "cc-O$l" "$o"
done

i=0
for b in $bins; do
	i=$((i + 1))
	"$b" "$reps" > "$out/run_$i.txt" 2>&1 || true
	awk '/checksum ok/ { print $1, $3 }
	     /^total:/      { print "total", $2 }' "$out/run_$i.txt" > "$out/data_$i.dat"
done

echo
echo "avg ms per scenario (reps=$reps), total in s:"
awk -v labels="$labels" '
	FNR == 1 { fi++ }
	{
		if (!($1 in seen)) { order[++nk] = $1; seen[$1] = 1 }
		v[$1, fi] = $2
	}
	END {
		nc = split(labels, L, " ")
		printf "%-11s", ""
		for (c = 1; c <= nc; c++) printf " %10s", L[c]
		printf "\n"
		for (r = 1; r <= nk; r++) {
			k = order[r]
			printf "%-11s", k
			for (c = 1; c <= nc; c++) {
				val = ((k SUBSEP c) in v) ? v[k, c] : "-"
				printf " %10s", val
			}
			printf "\n"
		}
	}' "$out"/data_*.dat
