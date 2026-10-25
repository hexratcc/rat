# rat [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)

<img align="right" src="./assets/emanuel.png" alt="emanuel" width="210">

rat is a simple, [**handmade**](https://handmade.network/manifesto), and *relatively* minimal compiler suite (including a [**C99 frontend**](./src/compiler/), a simple [**linker**](./src/linker/), and a [**Sea of Nodes**](https://en.wikipedia.org/wiki/Sea_of_nodes) [**backend**](./src/backend/)). Some of the features include but are not limited to:
- `rat -O1` compiles `sqlite3.c` about 17x faster than `clang -O1`)
- `rat -O1` code runs within 5% of `gcc -O1` and 26% of `clang -O1`.
- rat is small, readable and easy to hack on.

### running
```shell
$ make
$ make test
$ make bench
```
### optimization passes
- [**sccp**](./src/backend/pass/opt/sccp.cpp): sparse conditional constant propagation
- [**fold**](./src/backend/pass/opt/fold.cpp): constant folding, algebraic simplification
- [**simplifycfg**](./src/backend/pass/opt/simplify_cfg.cpp): control-flow simplification
- [**gvn**](./src/backend/pass/opt/gvn.cpp): global value numbering
- [**memoryopt**](./src/backend/pass/opt/memory_opt.cpp): load/store forwarding
- [**inline**](./src/backend/pass/opt/inline.cpp): function inlining
- [**strengthreduce**](./src/backend/pass/opt/strength_reduce.cpp): induction-variable strength reduction
- [**loadcanon**](./src/backend/pass/opt/load_canon.cpp): moves loads to their canonical memory state
- [**slp**](./src/backend/pass/opt/slp/): scheduling-free SLP vectorization with runtime alias guards
- [**dfe**](./src/backend/pass/opt/dead_func_elim.cpp): dead function elimination
### machine passes
- [**GCM**](./src/backend/codegen/schedule.cpp): global code motion
- [**x86-lower**](./src/backend/pass/emit/x86/x86_lower.cpp): instruction selection
- [**regalloc**](./src/backend/codegen/reg_alloc.cpp): bin-packing register allocation
- [**x86-peephole**](./src/backend/pass/emit/x86/x86_peephole.cpp): copy and spill cleanup
- [**x86-layout**](./src/backend/pass/emit/x86/x86_layout.cpp): block layout, loop alignment
- [**x86-encode**](./src/backend/pass/emit/x86/x86_encode.cpp): encoding and object emission
### tool passes
- [**verify**](./src/backend/pass/verify.cpp): IR invariant checks
- [**text-emitter**](./src/backend/pass/emit/text_emitter.cpp): IR dumps as Graphviz
- [**graph-emitter**](./src/backend/pass/emit/graph_emitter.cpp): IR dumps as text
