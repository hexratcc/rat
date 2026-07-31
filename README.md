# rat [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)
<img align="right" src="./assets/emanuel.png" alt="emanuel" width="140">Rat is a simple compiler backend focused on ease of use. It's inspired by [LLVM](https://llvm.org/) but focused on more novel approaches (ie. the [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) IR) and being much easier to understand (the backend library is currently ~13k LoC). To this end, rat focuses on bringing ~70% of [LLVM's](https://llvm.org/) performance with only a fraction of LLVM's complexity. You can find the documentation [here](./src/backend/README.md).

## running
```shell
$ ./build.sh
$ ./test.sh
$ ./bench.sh
```
