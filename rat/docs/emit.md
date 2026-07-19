## x86-64 backend
Two machine passes:
- [**x86-lower:**](../include/Pass/Emit/X86Lower.h) Schedules each function ([codegen.md](./codegen.md)) and lowers the graph to x86 machine instructions over virtual registers: instruction selection, phi resolution at block edges, the frame layout for allocs and spills, and calling-convention setup. Both the SysV and Windows x64 conventions are supported, selected by the target triple, integer values live in the GP class, floats in SSE, with an x87 class for 80-bit long double.
- [**x86-encode:**](../include/Pass/Emit/X86Encode.h) Runs after register allocation and encodes the machine instructions to bytes through the small assembler in [`Target/X86Asm.h`](../include/Target/X86Asm.h), emitting prologue/epilogue with callee-saved spills, resolving intra-function jumps via rel32 fixups, and laying out globals. Output goes into an `ObjectFile`.

## object files
rat exposes a simple object file abstraction ([`Target/ObjectFile.h`](../include/Target/ObjectFile.h)): A minimal writer with `text`, `rodata`, `data`, and `bss` sections, symbols, and three relocation kinds: `Abs64` (absolute 64-bit, `S + A`), `Pc32` (pc-relative, `S + A - P`, used by rip-relative lea), and `Plt32` (pc-relative call, `L + A - P`). Backed by [`X86Elf`](../include/Target/X86Elf.h) for linux and [`X86Coff`](../include/Target/X86Coff.h) for windows, link the result with any system linker.

## c emitter
[**c-emitter:**](../include/Pass/Emit/CEmitter.h) emits the module as portable C, using the same schedule as the x86 path: one labeled C block per scheduled block, phis lowered to assignments on the incoming edges, node values as numbered temporaries. Useful as a second backend for differential testing and as a bootstrapping escape hatch on targets without native support.

## visualization
- [**text-emitter:**](../include/Pass/Emit/TextEmitter.h) The textual IR form (ANSI-colorized); parseable back by `TextParser`, and the canonical form the IR test suite compares against.
- [**graph-emitter:**](../include/Pass/Emit/GraphEmitter.h) Graphviz DOT output, with per-edge-kind styling for control/memory/data edges. `rat -emit=dot foo.rat | dot -Tsvg` renders the graph. (TODO)
