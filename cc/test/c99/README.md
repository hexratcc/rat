# chibicc tests (imported)

These are the self-checking conformance tests from **chibicc**, a small C11
compiler:

- Upstream: https://github.com/rui314/chibicc
- Imported at commit `90d1f7f199cc55b13c7fdb5839d1409806633fdb`
- Copyright (c) 2019 Rui Ueyama and contributors
- License: MIT (see `LICENSE` in this directory)

## What these test

Each `*.c` file's `main` runs many `ASSERT(expected, actual)` checks and
returns 0 on success. A failing assertion prints a diagnostic and calls
`exit(1)`, so the program's exit status is the pass/fail signal. The ratcc test
harness (`test/cc/Runner.cpp`):

1. preprocesses the source with the host compiler's predefined macros and
   system include search paths;
2. emits C from ratcc's IR, compiles it with the host `cc`, runs it, and
   checks the program's return value against `// expect: 0`.

## Local modifications

The imported test bodies are byte-for-byte unchanged except for a single
leading `// expect: 0` directive line that the harness uses as the success
criterion.

## Curation

Not all of chibicc's tests are imported. The suite is restricted to tests
whose *subject* is ISO C. Files whose subject is a tool-specific extension
(GNU inline `asm`, `__attribute__`, `__builtin_*`, `alloca`, `__restrict__`,
GNU `case 0 ... 5:` ranges, `$` in identifiers) or that are coupled to
chibicc's own build layout (`#pragma once` self-include, computed `<>`
includes of `test/...`) are dropped, as are tests whose subject is a standard
feature ratcc does not yet implement (`_Atomic`, `_Generic`,
`_Alignof`/`_Alignas`, `_Thread_local`, `typeof`, binary `0b` literals).

Tests whose subject is standard C are kept even when ratcc cannot yet compile
them, so the failure documents a real frontend gap. Most of these are blocked
only by GNU statement expressions `({ ... })`, which chibicc uses purely as the
`ASSERT(...)` scaffolding around otherwise-standard code.

The one structural adaptation is `test.h`. Upstream compiles `test/common` as a
**separate** translation unit and links it with each test; the two never share
a translation unit, which is why `common` includes the real `<stdio.h>` while
the tests declare their own deliberately-incompatible `int printf(char *, ...)`
prototypes. This single-file harness has no separate-link step, so the contents
of `test/common` are inlined into `test.h` after the test prototypes, with the
upstream `#include <stdio.h>` / `#include <stdlib.h>` lines dropped (the minimal
prototypes are used instead) and `#include <stdarg.h>` kept (for `add_all`'s
`va_list`). The companion `include1.h`..`include4.h` headers used by `macro.c`
are copied verbatim.

## Known gaps

The kept tests were written to validate chibicc's own (permissive) semantics
and lean on GNU statement expressions `({ ... })` as the `ASSERT(...)`
scaffold; a few also use the chibicc-permissive implicit `int` (`const x;`,
`typedef int;`) or non-standard spellings the host `cc` oracle rejects
(e.g. `signed char signed`, `1f`, `sizeof(main)`). Files whose subject is
standard C but that hit one of these are expected to FAIL until the ratcc
frontend (and/or the oracle policy) grows to cover the construct, so that
progress stays visible as the frontend matures.
