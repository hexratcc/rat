## rat
Sea of Nodes compiler backend for toying around with various optimizations

## build
```shell
$ make        # build
$ make run    # build and runs the demo
$ make format # clang-format
```

## passes:
### optimization:
- **FoldPass:** Constant folding and algebraic simplification
- **GVNPass:** Global value numbering
- **SimplifyCFGPass:** Control-flow simplification
- **MemoryOptPass:** Load/store forwarding
### emit:
- **CEmitterPass:** Emit C code
### other:
- **VerifyPass:** Edge consistency + per-opcode structural invariants
- **TextEmitterPass:** Textual IR viz
- **GraphEmitterPass:** Graph IR viz
