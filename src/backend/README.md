# ir
A function body is a single [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) graph. There are no basic blocks in the IR itself; control flow, memory state, and data values are all just typed edges between nodes.

## edges
- **data:** Ordinary values (int/float/ptr). Free to float anywhere the schedule allows; order is implied purely by dependencies.
- **control:** Tokens that threads through `start -> if/region/... -> return`. Nodes that must execute at a particular point (load, store, call, terminators) take it as an input.
- **memory:** The heap as an SSA value. `store` consumes one memory state and produces the next, `load` consumes one, so independent memory chains can reorder freely while dependent ones can't.

Nodes that produce several results at once (`start`, `if`, `call`) have a tuple type; consumers reach individual elements through `proj` nodes. An `if` yields two control projections (then/else), a `call` yields control, memory, and optionally a value.

Every node keeps an ordered input list (defs) and a users list (reverse edges, one entry per using operand). All edge mutations (`addInput`, `setInput`, `replaceAllUsesWith`, ...) keeps the users index consistent. See [`IR/Node.h`](./ir/node.h) for more details.

## opcodes
Defined in [`IR/Opcode.h`](./ir/opcode.h), with per-opcode metadata (mnemonic, CFG/side-effect/commutativity flags, control-input slot, arity, class) in a single `OpcodeInfo` table.
- **control / structural:** `start`, `stop`, `return`, `region`, `if`, `proj`, `phi`
- **constants:** `const`
- **binary:** `add sub mul sdiv udiv srem urem and or xor shl lshr ashr`, `fadd fsub fmul fdiv`
- **unary:** `neg not fneg`
- **compare:** `eq ne slt sle ult ule`, `feq fne flt fle fgt fge`
- **convert:** `trunc sext zext`, `sitofp uitofp fptosi fptoui fpext fptrunc`
- **memory:** `load`, `store`
- **calls:** `call`
- **storage:** `global` (pointer to a module symbol), `alloc` (stack allocation)

## types
Types are interned in a `TypeContext` (the `Module` is one): `ctrl`, `mem`, `iN`, `fN`, `ptr`, arrays, and tuples for multi-result producers. `bool` is `i1`. See [`IR/Type.h`](./ir/type.h) for more details.

## building functions
A `Function` owns its nodes and exposes two layers of API ([`IR/Function.h`](./ir/function.h)):
- **graph layer:** `create<T>()` for raw node construction, iteration over all nodes, maintenance helpers for passes.
- **builder layer:** blocks, jumps, and named variables (`declareLocal` / `get` / `set`), so a frontend can emit straight-line code statement by statement and get a valid graph with phis already placed.

Emission happens into the current insertion block (`setInsertBlock`). A block collects its predecessor control edges as other blocks jump to it, and must be `seal()`ed once no further predecessors can appear. Sealing completes any pending phis. The memory state is threaded through the same variable mechanism, so loads and stores get the right memory edge for free.
```cpp
Module mod("demo");

// int clamp0(int x) { if (x < 0) x = 0; return x; }
Type* i32 = mod.getInt(32);
Function* fn = mod.createFunction("clamp0", {i32}, i32);

Function::Var x = fn->declareLocal("x", fn->param(0));
Function::Block* then = fn->createBlock();
Function::Block* join = fn->createBlock();

fn->jumpif(fn->compare(Opcode::Slt, fn->get(x), fn->constInt(i32, 0)), then);
fn->jmp(join);

fn->seal(then);
fn->setInsertBlock(then);
fn->set(x, fn->constInt(i32, 0));
fn->jmp(join);

fn->seal(join);
fn->setInsertBlock(join);
fn->ret(fn->get(x));

X86Target target;
PassManager pm(target);
pm.add<VerifyPass>(std::cerr);      // structural invariants
pm.add<TextEmitterPass>(std::cout); // print the graph
pm.run(mod);
```

## text format
`TextParser` and `TextEmitter` round-trip a stable textual form, used by the driver and the IR test suite:
```rs
func foldc() -> i32 {
	v0 = start : (ctrl, mem)
	v1 = stop : ctrl v7
	v2 = proj : ctrl  #0 "ctrl" of v0
	v3 = proj : mem  #1 "mem" of v0
	v4 = const : i32  3
	v5 = const : i32  2
	v6 = add : i32 v5, v4
	v7 = return : ctrl v2, v3, v6
}
```
One node per line: `vN = <mnemonic> : <type> <operands>`. Constants carry their value, projections their index and label, calls their callee, globals their symbol. Module-level data appears above functions as `const name : type = "bytes"` / `var name : type = "bytes"`.

## invariants
`VerifyPass` ([`Pass/Verify.h`](./pass/verify.h)) checks edge consistency (every input lists the node as a user and vice versa, no cross-function edges, no nulls) plus per-opcode structural invariants: arity, tuple shapes of `start`/`if`/`call`, operand kinds (control/memory/data in the right slots), unique `start`/`stop`, and that `stop` collects exactly the function's returns.

# passes
Passes come in three kinds: module `Pass`, `FunctionPass` (run per function), and `MachinePass` (post-lowering, over machine state). The `PassManager` runs all IR passes first, then machine passes, in insertion order, and can report per-pass timing. For more info see [`Pass/Pass.h`](./pass/pass.h).

## optimization
- [**fold:**](./pass/opt/fold.h) Peephole constant folding and algebraic simplification, applied as local graph rewrites. Covers constant arithmetic, identities (`x + 0`, `x * 1`, `x & x`, `x ^ x`, ...), reassociation of constant chains, strength reduction (`mul`/`udiv`/`urem` by powers of two into shifts/masks), shift-of-shift collapse, and constant compares/converts.
- [**gvn:**](./pass/opt/gvn.h) Global value numbering. Hash-conses congruent pure value nodes so equal computations share a single node.
- [**sccp:**](./pass/opt/sccp.h) Sparse conditional constant propagation. An optimistic data-flow solver over a top/constant/bottom lattice that jointly discovers constants and which control edges can execute, so a value merged at a region (phi) is constant whenever every *reachable* predecessor agrees. The rewrite materializes proven constants (branch predicates included) and drops dead nodes; pruning the dead branch side is left to `simplifycfg`.
- [**simplifycfg:**](./pass/opt/simplify_cfg.h) Control-flow simplification. Folds branches on constant predicates, collapses single-predecessor regions and their phis, and prunes unreachable control.
- [**memoryopt:**](./pass/opt/memory_opt.h) Store-to-load forwarding and redundant-load elimination over the explicit memory edges, disambiguated by [alias analysis](./pass/opt/alias_analysis.h). A load's effective def is found by skipping back over stores that provably don't alias `[addr, addr + size)`; must-alias stores forward their value, and loads with the same effective def and address CSE. The alias analysis decomposes addresses into `base + constant + symbolic addends`, answers no/may/must for two accesses, and knows that distinct identified objects (two allocs, two globals, alloc vs global) never alias.
- [**inline:**](./pass/opt/inline.h) Function inlining. Replaces a call to a small, non-recursive callee with a clone of its body, splicing the callee's control and memory edges into the caller and merging its returns at the call's continuation. Budgets: callee <= 64 nodes, <= 256 inlines per caller, caller may grow by <= 192 nodes.
- [**dfe:**](./pass/opt/dead_func_elim.h) Dead function elimination. Drops internal (`static`) functions with no direct callers whose address is never taken. External linkage is always kept. Iterated to a fixpoint so chains of internal helpers fall away together.

## utility
- [**verify:**](./pass/verify.h) Edge consistency + per-opcode structural invariants (see [ir](#ir)).
- [**rename-symbol:**](./pass/opt/rename_symbol.h) Rename a function or global and every reference to it.

Visualization passes (`text-emitter`, `graph-emitter`, `c-emitter`) are covered in [x86-64 backend](#x86-64-backend).

## default pipeline
`-O1` runs, in order:
```rs
sccp, fold, simplifycfg, gvn, memoryopt, inline, fold, gvn, strengthreduce, fold, gvn, slp, fold, gvn, dfe
```

# codegen
Code generation turns the floating graph back into linear code in three stages: schedule the graph into blocks, lower to a target-independent machine IR, allocate registers. Only the lowering step is target-specific.

## schedule
[`CodeGen/Schedule.h`:](./codegen/schedule.h) global code motion. Recovers a CFG from the control edges (one block per region / entry projection / if projection, terminated by a return, a two-way branch, or a goto), computes dominators with Lengauer-Tarjan and loop depth from natural loops, then places each floating node into a block: as early as its inputs allow, then sunk toward its uses into the shallowest-loop-depth block dominated by the early placement (hoisting out of loops where legal). Blocks come out in RPO with their phis and compute nodes in emit order. Used by both the x86 backend and the C emitter.

## machine ir
[`CodeGen/MachineFunction.h:`](./codegen/machine_function.h) a minimal, target-independent instruction form: blocks of `MachineInstr`s with defs, uses, clobbers, a register class, and backend-defined immediates. Operands are virtual registers, physical registers, immediates, frame slots, symbols, or block references. Calls are flagged so allocators apply clobbers and bound live ranges correctly.

## register allocation
The allocator builds on [`CodeGen/RegAllocBase.h`](./codegen/reg_alloc_base.h): per-block liveness over dense vreg bitsets, a linearized instruction order, copy-hint collection, and a common rewrite step that patches assignments in and inserts spills/reloads. It is fully backend-agnostic (register classes come from the target's `RegisterInfo`, and spill/reload/slot construction goes through `RegAllocHooks` callbacks), so the allocator never names a single target opcode.
- [**linear scan:**](./codegen/linear_scan_reg_alloc.h) Live ranges are hole-aware segment lists: the gaps between segments are provably off every def-use path, so fixed-register pins inside a hole don't constrain the value and call clobbers inside a hole don't force a callee-saved register. Assignment scans ranges in start order per class. Copy hints bias the choice so coalescable moves become elided self-moves. Under pressure a value spills to a frame slot.

# x86-64 backend
Two machine passes:
- [**x86-lower:**](./pass/emit/x86/x86_lower.h) Schedules each function ([codegen](#codegen)) and lowers the graph to x86 machine instructions over virtual registers: instruction selection, phi resolution at block edges, the frame layout for allocs and spills, and calling-convention setup. Integer values live in the GP class, floats in SSE, with an x87 class for 80-bit long double.
- [**x86-encode:**](./pass/emit/x86/x86_encode.h) Runs after register allocation and encodes the machine instructions to bytes through the small assembler in [`Target/X86Asm.h`](./target/x86/x86_asm.h), emitting prologue/epilogue with callee-saved spills (page-probed on windows), resolving intra-function jumps via rel32 fixups, and laying out globals. Output goes into an `ObjectFile`.

## calling conventions
Both the SysV (linux) and Windows x64 conventions are supported, selected by the target triple. Every OS-specific ABI rule (argument registers), shared vs split argument slots, shadow space, stack parameter offsets, x87 by-ref passing, va_list model, register save areas, callee-saved sets (lives in one constexpr table ([`X86CallConv`](./target/x86/x86_asm.h))), with one instance per OS (`abi::kSysV`, `abi::kWin64`). The lowering and encoding passes are written once against the descriptor:
- **`X86ArgAssigner`:** Walks arguments in declaration order and yields register-or-stack locations under either slot model, shared by call lowering, the prologue, and variadic frame layout.
- **Derived register file:** Allocation order, callee-saved sets, and call clobbers (allocatable minus callee-saved, plus scratch) are all built from the same table, so adding a convention means adding a row, not a code path.

## object files
rat exposes a simple object file abstraction ([`Target/ObjectFile.h`](./target/object_file.h)): A minimal writer with `text`, `rodata`, `data`, and `bss` sections, symbols, and three relocation kinds: `Abs64` (absolute 64-bit, `S + A`), `Pc32` (pc-relative, `S + A - P`, used by rip-relative lea), and `Plt32` (pc-relative call, `L + A - P`). Backed by [`X86Elf`](./target/x86/x86_elf.cpp) for linux and [`X86Coff`](./target/x86/x86_coff.cpp) for windows, link the result with any system linker.

## c emitter
[**c-emitter:**](./pass/emit/c/c_emitter.h) emits the module as portable C, using the same schedule as the x86 path: one labeled C block per scheduled block, phis lowered to assignments on the incoming edges, node values as numbered temporaries. Useful as a second backend for differential testing and as a bootstrapping escape hatch on targets without native support.

## visualization
- [**text-emitter:**](./pass/emit/text_emitter.h) The textual IR form (ANSI-colorized); parseable back by `TextParser`, and the canonical form the IR test suite compares against.
- [**graph-emitter:**](./pass/emit/graph_emitter.h) Graphviz DOT output, with per-edge-kind styling for control/memory/data edges. `rat -emit=dot foo.rat | dot -Tsvg` renders the graph. (TODO)

# testing

## IR tests (`src/backend/test/*.rat`)
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

## compiler tests (`src/compiler/test/**/*.c`)
End-to-end: each case is compiled by the compiler alone (using its builtin predefined macros and bundled standard headers), then linked with the host compiler, executed, and its exit code (and optionally stdout) checked. Alongside the local cases (`custom/`) the suite carries external corpora ([c-testsuite](https://github.com/c-testsuite/c-testsuite), gcc-torture, c99) with directives added. The suite runs `cc-x86` through the native backend.

Directives are comments:
- `// expect: N`: required; expected exit code (`main`'s return)
- `// expect-<os>: N`: os-specific override (`linux`/`windows`)
- `// passes: a,b,...`: pipeline for this case (default: the `-O1` pipeline)
- `// output:`: expected stdout follows on `//| ...` continuation lines
- `// skip-target: <os>`, `// skip-x86-target: <os>`

When no output is expected and the optimized `main` reduces to `return <const>`, the runner reads the result straight out of the IR instead of executing, as the passes must have proven the answer at compile time.

