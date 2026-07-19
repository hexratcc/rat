# testing
Two data-driven suites share a small multithreaded harness ([`Support/TestHarness.h`](../include/Support/TestHarness.h)), `ctest` runs both. Cases are plain files with directives, so adding a test is adding a file.

## IR tests (`rat/test/*.rat`)
Run a pass pipeline over textual IR and compare against expected IR:
```rust
@name fold: constant arithmetic folds away
@passes fold

@input
func foldc() -> i32 {
	...
}

@expect
func foldc() -> i32 {
	...
}
```
Both sides are parsed and re-emitted through `TextEmitter`, so the comparison is on canonical form (node numbering and whitespace don't have to match). Run the suite with `./bin/rat-test`, to poke at a pipeline interactively, feed raw textual IR (a test's `@input` body) to `./bin/rat -passes=...`.

## cc tests (`cc/test/**/*.c`)
End-to-end: each case is compiled, linked with the host compiler, executed, and its exit code (and optionally stdout) checked. Alongside the local cases the suite carries external corpora ([c-testsuite](https://github.com/c-testsuite/c-testsuite), gcc-torture, c99) with directives added. The same suite runs twice (`cc-x86` through the native backend and `cc-c` through the C emitter (`RATCC_X86=0`)), and both backends must agree. `RATCC_REGALLOC=graph` switches the allocator.

Directives are comments:
- `// expect: N`: required; expected exit code (`main`'s return)
- `// expect-<os>: N`: os-specific override (`linux`/`windows`)
- `// passes: a,b,...`: pipeline for this case (default: the `-O1` pipeline)
- `// output:`: expected stdout follows on `//| ...` continuation lines
- `// output: oracle`: compile the same source with the host compiler and require identical stdout
- `// skip-target: <os>`, `// skip-x86-target: <os>`

When no output is expected and the optimized `main` reduces to `return <const>`, the runner reads the result straight out of the IR instead of executing, as the passes must have proven the answer at compile time.
