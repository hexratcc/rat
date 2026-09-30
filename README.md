# rat [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)

<img align="right" src="./assets/emanuel.png" alt="emanuel" width="160">

**warning: wip**

rat is a simple [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) compiler backend, which aims to be reasonably fast, while being relatively simple (the core is currently about 15k LoC). As a proof of concept of the backend, I'm working on a C99 frontend for it which can be found [here](./src/compiler/). The frontend is about 1.3x slower than gcc in terms of runtime, but has about 10x faster compile times.

## running
```shell
$ make
$ make test
$ make bench
```

<!-- ## performance
![perf](https://raw.githubusercontent.com/hexratcc/rat/perf/perf.png)
![compile](https://raw.githubusercontent.com/hexratcc/rat/perf/compile.png) -->
