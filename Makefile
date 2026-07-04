# make           build the core lib and bin/rat pipeline driver
# make rat       build the bin/rat pipeline driver
# make test      run both suites (cc via x86-64 backend, plus rat IR)
# make test-c    run the cc suite through the C backend, plus rat IR
# make test-x86  run the cc suite through the x86-64 backend, plus rat IR
# make test-rat  run only the rat IR golden suite
# make compiledb generate compile_commands.json for editors
# make format    run clang-format over the sources
# make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -Irat/include
DEPFLAGS := -MMD -MP

LIB_SRCS := $(shell find rat/lib -name '*.cpp' | sort)

LIB_OBJS := $(patsubst %.cpp,build/%.o,$(LIB_SRCS))
LIB      := build/rat.a

SOURCES  := $(LIB_SRCS) rat/test/driver.cpp rat/test/Runner.cpp
HEADERS  := $(wildcard rat/include/*.h rat/include/IR/*.h rat/include/Support/*.h rat/include/Pass/*.h rat/include/Pass/Emit/*.h rat/include/Pass/Opt/*.h rat/include/CodeGen/*.h rat/include/Target/*.h)
CC_FMT   := $(shell find cc \( -name '*.cpp' -o -name '*.h' \) -print | sort)

.PHONY: all rat test test-c test-x86 test-rat compiledb format clean
all: compiledb bin/rat

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(LIB_OBJS:.o=.d)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

bin/rat: $(LIB)
	$(MAKE) -C rat/test all

rat: bin/rat

test-rat:
	$(MAKE) -C rat/test test TESTBIN_ARGS="-j$$(nproc)"

test: test-rat
	$(MAKE) -C cc test TESTBIN_ARGS="-j$$(nproc)"

test-c: test-rat
	$(MAKE) -C cc test-c TESTBIN_ARGS="-j$$(nproc)"

test-x86: test-rat
	$(MAKE) -C cc test-x86 TESTBIN_ARGS="-j$$(nproc)"

compiledb:
	@printf '[\n' > compile_commands.json
	@first=1; for f in $(SOURCES); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; first=0; \
		printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s %s -c %s"\n  }' \
			"$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$(INC)" "$$f" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json

format:
	clang-format -i $(SOURCES) $(HEADERS) $(CC_FMT)

clean:
	rm -rf build bin
