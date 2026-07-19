# passes
Passes come in three kinds: module `Pass`, `FunctionPass` (run per function), and `MachinePass` (post-lowering, over machine state). The `PassManager` runs all IR passes first, then machine passes, in insertion order, and can report per-pass timing. For more info see [`Support/Pass.h`](../include/Support/Pass.h).

## optimization
- [**fold:**](../include/Pass/Opt/Fold.h) Peephole constant folding and algebraic simplification, applied as local graph rewrites. Covers constant arithmetic, identities (`x + 0`, `x * 1`, `x & x`, `x ^ x`, ...), reassociation of constant chains, strength reduction (`mul`/`udiv`/`urem` by powers of two into shifts/masks), shift-of-shift collapse, and constant compares/converts.
- [**gvn:**](../include/Pass/Opt/GVN.h) Global value numbering. Hash-conses congruent pure value nodes so equal computations share a single node.
- [**sccp:**](../include/Pass/Opt/SCCP.h) Sparse conditional constant propagation. An optimistic data-flow solver over a top/constant/bottom lattice that jointly discovers constants and which control edges can execute, so a value merged at a region (phi) is constant whenever every *reachable* predecessor agrees. The rewrite materializes proven constants (branch predicates included) and drops dead nodes; pruning the dead branch side is left to `simplifycfg`.
- [**simplifycfg:**](../include/Pass/Opt/SimplifyCFG.h) Control-flow simplification. Folds branches on constant predicates, collapses single-predecessor regions and their phis, and prunes unreachable control.
- [**memoryopt:**](../include/Pass/Opt/MemoryOpt.h) Store-to-load forwarding and redundant-load elimination over the explicit memory edges, disambiguated by [alias analysis](../include/Pass/Opt/AliasAnalysis.h). A load's effective def is found by skipping back over stores that provably don't alias `[addr, addr + size)`; must-alias stores forward their value, and loads with the same effective def and address CSE. The alias analysis decomposes addresses into `base + constant + symbolic addends`, answers no/may/must for two accesses, and knows that distinct identified objects (two allocs, two globals, alloc vs global) never alias.
- [**inline:**](../include/Pass/Opt/Inline.h) Function inlining. Replaces a call to a small, non-recursive callee with a clone of its body, splicing the callee's control and memory edges into the caller and merging its returns at the call's continuation. Budgets: callee ≤ 64 nodes, ≤ 256 inlines per caller, caller may grow by ≤ 192 nodes.
- [**dfe:**](../include/Pass/Opt/DeadFuncElim.h) Dead function elimination. Drops internal (`static`) functions with no direct callers whose address is never taken. External linkage is always kept. Iterated to a fixpoint so chains of internal helpers fall away together.

## utility
- [**verify:**](../include/Pass/Verify.h) Edge consistency + per-opcode structural invariants (see [ir.md](./ir.md)).
- [**rename-symbol:**](../include/Pass/Opt/RenameSymbol.h) Rename a function or global and every reference to it.

Visualization passes (`text-emitter`, `graph-emitter`, `c-emitter`) are covered in [emit.md](./emit.md).

## default pipeline
`-O1` runs, in order:
```rs
sccp, fold, simplifycfg, gvn, memoryopt, inline, fold, gvn, dfe
```
