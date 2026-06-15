## rat
Sea of Nodes compiler backend for toying around with various optimizations

## build
```shell
$ make        # build
$ make run    # build and runs the demo
$ make format # clang-format
```

## passes:
- [**FoldPass:**](./include/Pass/Opt/Fold.h) Constant folding and algebraic simplification
- [**GVNPass:**](./include/Pass/Opt/GVN.h) Global value numbering
- [**SimplifyCFGPass:**](./include/Pass/Opt/SimplifyCFG.h) Control-flow simplification
- [**MemoryOptPass:**](./include/Pass/Opt/MemoryOpt.h) Load/store forwarding
- [**CEmitterPass:**](./include/Pass/Emit/CEmitter.h) Emit C code
- [**VerifyPass:**](./include/Pass/Verify.h) Edge consistency + per-opcode structural invariants
- [**TextEmitterPass:**](./include/Pass/Emit/TextEmitter.h) Textual IR viz
- [**GraphEmitterPass:**](./include/Pass/Emit/GraphEmitter.h) Graph IR viz
