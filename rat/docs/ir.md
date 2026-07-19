# ir
A function body is a single [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) graph. There are no basic blocks in the IR itself; control flow, memory state, and data values are all just typed edges between nodes.

## edges
- **data:** Ordinary values (int/float/ptr). Free to float anywhere the schedule allows; order is implied purely by dependencies.
- **control:** Tokens that threads through `start -> if/region/... -> return`. Nodes that must execute at a particular point (load, store, call, terminators) take it as an input.
- **memory:** The heap as an SSA value. `store` consumes one memory state and produces the next, `load` consumes one, so independent memory chains can reorder freely while dependent ones can't.

Nodes that produce several results at once (`start`, `if`, `call`) have a tuple type; consumers reach individual elements through `proj` nodes. An `if` yields two control projections (then/else), a `call` yields control, memory, and optionally a value.

Every node keeps an ordered input list (defs) and a users list (reverse edges, one entry per using operand). All edge mutations (`addInput`, `setInput`, `replaceAllUsesWith`, ...) keeps the users index consistent. See [`IR/Node.h`](../include/IR/Node.h) for more details.

## opcodes
Defined in [`IR/Opcode.h`](../include/IR/Opcode.h), with per-opcode metadata (mnemonic, CFG/side-effect/commutativity flags, control-input slot, arity, class) in a single `OpcodeInfo` table.
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
Types are interned in a `TypeContext` (the `Module` is one): `ctrl`, `mem`, `iN`, `fN`, `ptr`, arrays, and tuples for multi-result producers. `bool` is `i1`. See [`IR/Type.h`](../include/IR/Type.h) for more details.

## building functions
A `Function` owns its nodes and exposes two layers of API ([`IR/Function.h`](../include/IR/Function.h)):
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
`VerifyPass` ([`Pass/Verify.h`](../include/Pass/Verify.h)) checks edge consistency (every input lists the node as a user and vice versa, no cross-function edges, no nulls) plus per-opcode structural invariants: arity, tuple shapes of `start`/`if`/`call`, operand kinds (control/memory/data in the right slots), unique `start`/`stop`, and that `stop` collects exactly the function's returns.
