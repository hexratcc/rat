# rat [![test (ubuntu-latest)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml/badge.svg)](https://github.com/hexratcc/rat/actions/workflows/test_ubuntu_latest.yml)

<img align="right" src="./assets/emanuel.png" alt="emanuel" width="160">

**warning: wip**

rat is a simple [Sea of Nodes](https://en.wikipedia.org/wiki/Sea_of_nodes) compiler backend, which aims to be reasonably fast, while not being large (the core is currently about 15k LoC). The suite also contains a basic C99 frontend, along with a basic linker. You can find the documentation [here](./src/backend/README.md).

## running
```shell
$ make
$ make test
$ make bench
```

<!-- ## performance
![perf](https://raw.githubusercontent.com/hexratcc/rat/perf/perf.png)
![compile](https://raw.githubusercontent.com/hexratcc/rat/perf/compile.png) -->
