# rat [![openssf](https://api.securityscorecards.dev/projects/github.com/hexratcc/rat/badge)](https://securityscorecards.dev/viewer/?uri=github.com/hexratcc/rat) [![test (windows-latest)](https://github.com/hexratcc/rat/actions/workflows/test_windows_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_windows_latest.yml) [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)
<img align="right" src="./assets/emanuel.png" alt="emanuel" width="80">Rat is a simple compiler backend focused on ease of use. It's inspired by [LLVM](https://llvm.org/) but focused on more novel approaches (ie. the [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) IR) and being much easier to understand (the backend library is currently ~13k LoC). To this end, rat focuses on bringing ~70% of [LLVM's](https://llvm.org/) performance with only a fraction of LLVM's complexity. You can find the documentation [here](./rat/README.md).

## build
```shell
$ cmake -B build
$ cmake --build build -j # build
$ ctest --test-dir build # run tests
```
