#!/usr/bin/env bash
# slpcov.sh: per-kernel BB-SLP coverage table
# usage: slpcov.sh <ratcc> [hostcc-link]
#
# compiles slpcov.c three ways (rat -fslp, gcc -O2, clang -O2) and marks each
# kernel 1/0 for having any packed SSE/AVX op in its disassembly
set -eu
cd "$(dirname "$0")"

ratcc=${1:-../../../bin/cc}
link=${2:-cc}
src=slpcov.c
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

"$ratcc" -O1 -fslp -o "$out/rat.o" "$src"
"$link" -no-pie "$out/rat.o" -o "$out/rat" -lm
gcc   -std=c99 -O2 "$src" -o "$out/gcc"   -lm
clang -std=c99 -O2 "$src" -o "$out/clang" -lm

kernels=$(grep -oE '^NOINLINE void [a-z0-9_]+' "$src" | awk '{print $3}')
fmt='%-26s %5s %5s %6s\n'

# packed SSE/AVX ops: moves, integer arith, float arith, avx forms
packed='movups|movdqu|movaps|movdqa'
packed+='|paddd|paddq|psubd|psubq|pand|por|pxor'
packed+='|addps|subps|mulps|divps|addpd|subpd|mulpd|divpd'
packed+='|vmov(u|a)p|vpadd|vpsub|vmulp|vaddp'

# 1 if kernel $2 has any packed op in binary $1
vec() { objdump -d "$1" | sed -n "/<$2>:/,/^$/p" | grep -qE "$packed" && echo 1 || echo 0; }

printf "$fmt" kernel rat gcc clang
rt=0; gt=0; ct=0; n=0
for k in $kernels; do
	r=$(vec "$out/rat" "$k"); g=$(vec "$out/gcc" "$k"); c=$(vec "$out/clang" "$k")
	printf "$fmt" "$k" "$r" "$g" "$c"
	rt=$((rt+r)); gt=$((gt+g)); ct=$((ct+c)); n=$((n+1))
done
printf "$fmt" "TOTAL/$n" "$rt" "$gt" "$ct"
