#!/usr/bin/env bash
# usage: ./test.sh

set -euo pipefail
cd "$(dirname "$0")"

[ -x bin/rat-test ] && [ -x bin/cc-test ] || { echo "missing binaries, run ./build.sh" >&2; exit 1; }

J="-j$(nproc)"
rc=0

echo "== rat-ir =="
( cd src/backend  && ../../bin/rat-test $J ) || rc=1

echo "== cc-x86 =="
( cd src/compiler && RATCC_X86=1 ../../bin/cc-test $J ) || rc=1

echo "== cc-c =="
( cd src/compiler && RATCC_X86=0 ../../bin/cc-test $J ) || rc=1

exit $rc
