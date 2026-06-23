## rat
Compiler backend for toying around with various optimizations

## build
```shell
$ make        # build
$ make run    # build and runs the demo
$ make test   # builds and runs tests
```

## passes
- [**FoldPass:**](./include/Pass/Opt/Fold.h) Constant folding and algebraic simplification
- [**GVNPass:**](./include/Pass/Opt/GVN.h) Global value numbering
- [**SCCPPass:**](./include/Pass/Opt/SCCP.h) Sparse conditional constant propagation
- [**SimplifyCFGPass:**](./include/Pass/Opt/SimplifyCFG.h) Control-flow simplification
- [**MemoryOptPass:**](./include/Pass/Opt/MemoryOpt.h) Load/store forwarding
- [**InlinePass:**](./include/Pass/Opt/Inline.h) Function inlining
- [**CEmitterPass:**](./include/Pass/Emit/CEmitter.h) Emit C code
- [**VerifyPass:**](./include/Pass/Verify.h) Edge consistency + per-opcode structural invariants
- [**TextEmitterPass:**](./include/Pass/Emit/TextEmitter.h) Textual IR viz
- [**GraphEmitterPass:**](./include/Pass/Emit/GraphEmitter.h) Graph IR viz
- [**RenameSymbolPass:**](./include/Pass/Opt/RenameSymbolPass.h) Rename a given symbol