## rat [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg?branch=main)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)


<img align="left" src="./assets/emanuel.png" alt="emanuel" width="100">Rat is a simple compiler backend focused on ease of use. It's inspired by [LLVM](https://llvm.org/) but focused on more novel approaches (ie. the [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) IR) and being much easier to understand (rat is currently only about 20k LoC). To this end, rat focuses on bringing ~70% of [LLVM's](https://llvm.org/) performance with only a fraction of LLVM's complexity.

## build
```shell
$ make        # build
$ make test   # builds and runs tests
```

## passes
**optimization**
- [**FoldPass:**](./rat/include/Pass/Opt/Fold.h) Constant folding and algebraic simplification
- [**GVNPass:**](./rat/include/Pass/Opt/GVN.h) Global value numbering
- [**SCCPPass:**](./rat/include/Pass/Opt/SCCP.h) Sparse conditional constant propagation
- [**SimplifyCFGPass:**](./rat/include/Pass/Opt/SimplifyCFG.h) Control-flow simplification
- [**MemoryOptPass:**](./rat/include/Pass/Opt/MemoryOpt.h) Load/store forwarding
- [**InlinePass:**](./rat/include/Pass/Opt/Inline.h) Function inlining

**codegen**
- [**X86LowerPass:**](./rat/include/Pass/Emit/X86Emitter.h) Lower IR to x86 machine instructions
- [**X86EncodePass:**](./rat/include/Pass/Emit/X86Emitter.h) Encode x86 to an ELF object
- [**CEmitterPass:**](./rat/include/Pass/Emit/CEmitter.h) Emit C code

**utility**
- [**VerifyPass:**](./rat/include/Pass/Verify.h) Edge consistency + per-opcode structural invariants
- [**RenameSymbolPass:**](./rat/include/Pass/Opt/RenameSymbol.h) Rename a given symbol
- [**TextEmitterPass:**](./rat/include/Pass/Emit/TextEmitter.h) Textual IR viz
- [**GraphEmitterPass:**](./rat/include/Pass/Emit/GraphEmitter.h) Graphviz DOT IR viz