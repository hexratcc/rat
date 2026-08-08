#!/usr/bin/env bash
# usage: slpext.sh <ratcc> [hostcc-link]
set -eu
cd "$(dirname "$0")"

ratcc=${1:-../../../bin/cc}
link=${2:-cc}
src=slpext.c
runs=${RUNS:-3}
reps=${REPS:-20000}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

gccflags="-std=c99 -O3 -fno-tree-loop-vectorize -fno-unroll-loops -fno-peel-loops \
  -fno-builtin -fno-tree-loop-distribute-patterns -march=x86-64-v2"
clangflags="-std=c99 -O3 -fno-vectorize -fno-unroll-loops -fno-builtin -march=x86-64-v2"

"$ratcc" -O1 -o "$out/rat_scal.o" "$src"
"$ratcc" -O1 -fslp -o "$out/rat_slp.o" "$src"
"$link" -no-pie "$out/rat_scal.o" -o "$out/rat_scal" -lm
"$link" -no-pie "$out/rat_slp.o" -o "$out/rat_slp" -lm
gcc $gccflags -fno-tree-slp-vectorize "$src" -o "$out/gcc_scal" -lm
gcc $gccflags -ftree-slp-vectorize "$src" -o "$out/gcc_slp" -lm
clang $clangflags -fno-slp-vectorize "$src" -o "$out/clang_scal" -lm
clang $clangflags -fslp-vectorize "$src" -o "$out/clang_slp" -lm

kernels=$(grep -oE '^static NOINLINE [a-z0-9]+ e[0-9]+[a-z0-9_]+' "$src" | awk '{print $4}')

# coverage
# mov family counts only with a memory operand (reg-reg movaps is scalar plumbing)
packedmov='(movups|movdqu|movaps|movdqa).*\('
packedalu='paddd|paddq|psubd|psubq|pmulld|pmuludq'
packedalu+='|addps|subps|mulps|divps|addpd|subpd|mulpd|divpd|haddps|addsubps'
vec() {
	objdump -d "$1" | sed -n "/<$2>:/,/^$/p" \
	| grep -qE "$packedmov|$packedalu" && echo 1 || echo 0
}

fmt='%-18s %5s %5s %6s\n'
printf "$fmt" kernel rat gcc clang
rt=0; gt=0; ct=0; n=0
for k in $kernels; do
	r=$(vec "$out/rat_slp" "$k"); g=$(vec "$out/gcc_slp" "$k"); c=$(vec "$out/clang_slp" "$k")
	printf "$fmt" "$k" "$r" "$g" "$c"
	rt=$((rt+r)); gt=$((gt+g)); ct=$((ct+c)); n=$((n+1))
done
printf "$fmt" "TOTAL/$n" "$rt" "$gt" "$ct"
echo

# timing: median of $runs alternated sweeps, one column per build
builds="rat_scal rat_slp gcc_scal gcc_slp clang_scal clang_slp"
for i in $(seq "$runs"); do
	for b in $builds; do
		"$out/$b" "$reps" > "$out/$b.run$i"
	done
done
for b in $builds; do
	awk '/^e[0-9]/ { t[$1] = t[$1] " " $2 } END { for(k in t) print k t[k] }' "$out/$b".run* \
	| while read -r k vals; do
		med=$(echo "$vals" | tr ' ' '\n' | sort -g | awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }')
		echo "$k $med"
	done | sort > "$out/$b.med"
done

checks=$(cat "$out"/*.run1 | awk '/^check/ { print $2 }' | sort -u | wc -l)
[ "$checks" = 1 ] || echo "WARN: checksums differ across builds"

join "$out/rat_scal.med" "$out/rat_slp.med" > "$out/j1"
join "$out/j1" "$out/gcc_scal.med" > "$out/j2"
join "$out/j2" "$out/gcc_slp.med" > "$out/j3"
join "$out/j3" "$out/clang_scal.med" > "$out/j4"
join "$out/j4" "$out/clang_slp.med" \
| awk 'BEGIN {
		printf "%-18s %9s %9s %6s %9s %9s %6s %9s %9s %6s\n",
			"kernel", "rat", "rat+slp", "x", "gcc", "gcc+slp", "x", "clang", "clang+slp", "x"
	}
	{
		rs = $2 / $3; gs = $4 / $5; cs = $6 / $7
		lr += log(rs); lg += log(gs); lc += log(cs); n++
		printf "%-18s %9.3f %9.3f %6.2f %9.3f %9.3f %6.2f %9.3f %9.3f %6.2f\n",
			$1, $2, $3, rs, $4, $5, gs, $6, $7, cs
	}
	END {
		printf "%-18s %9s %9s %6.2f %9s %9s %6.2f %9s %9s %6.2f\n",
			"GEOMEAN", "", "", exp(lr / n), "", "", exp(lg / n), "", "", exp(lc / n)
	}'
