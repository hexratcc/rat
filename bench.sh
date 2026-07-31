#!/usr/bin/env bash
# usage: ./bench.sh [reps]
set -euo pipefail
cd "$(dirname "$0")"

REPS="${1:-5}"
HOSTCC="${HOSTCC:-cc}"

[ -x bin/cc ] || { echo "missing bin/cc, run ./build.sh" >&2; exit 1; }

src/compiler/bench/compare.sh \
	bin/cc "$HOSTCC" \
	src/compiler/bench/bench.c \
	build/bench-out "$REPS"
