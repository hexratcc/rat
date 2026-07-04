# make           build both projects (rat + cc)
# make rat       build the rat core lib, driver, and harness
# make cc        build the cc frontend and harness
# make test      run both suites (cc via x86-64 backend, plus rat IR)
# make test-c    run the cc suite through the C backend, plus rat IR
# make test-x86  run the cc suite through the x86-64 backend, plus rat IR
# make test-rat  run only the rat IR golden suite
# make compiledb generate compile_commands.json for editors
# make format    run clang-format over all sources
# make clean     remove all build artifacts

JOBS := -j$(shell nproc)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

RAT_SRCS := $(shell find rat/lib rat/test -name '*.cpp' | sort)
CC_SRCS  := $(shell find cc/lib -name '*.cpp' | sort) cc/main.cpp cc/Runner.cpp

.PHONY: all rat cc test test-c test-x86 test-rat compiledb format clean
all: rat cc

rat:
	$(MAKE) -C rat all

cc: rat
	$(MAKE) -C cc all

test-rat:
	$(MAKE) -C rat test TESTBIN_ARGS="$(JOBS)"

test: test-rat
	$(MAKE) -C cc test TESTBIN_ARGS="$(JOBS)"

test-c: test-rat
	$(MAKE) -C cc test-c TESTBIN_ARGS="$(JOBS)"

test-x86: test-rat
	$(MAKE) -C cc test-x86 TESTBIN_ARGS="$(JOBS)"

compiledb:
	@printf '[\n' > compile_commands.json
	@first=1; for f in $(RAT_SRCS); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; first=0; \
		printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s -Irat/include -c %s"\n  }' \
			"$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$$f" >> compile_commands.json; \
	done; \
	for f in $(CC_SRCS); do \
		printf ',\n' >> compile_commands.json; \
		printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s -Irat/include -Icc/include -c %s"\n  }' \
			"$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$$f" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json

format:
	$(MAKE) -C rat format
	$(MAKE) -C cc format

clean:
	$(MAKE) -C rat clean
	$(MAKE) -C cc clean
	rm -rf build bin
