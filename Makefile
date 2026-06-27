# make           build bin/tour and bin/rat
# make run       build and run the tour example
# make rat       build the bin/rat pipeline driver
# make test      run tests (x86-64 backend)
# make test-c    run tests through the C backend
# make test-x86  run tests through the x86-64 backend
# make compiledb generate compile_commands.json for editors
# make format    run clang-format over the sources
# make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -Iinclude
DEPFLAGS := -MMD -MP

LIB_SRCS := $(shell find lib -name '*.cpp' | sort)

LIB_OBJS := $(patsubst %.cpp,build/%.o,$(LIB_SRCS))
LIB      := build/rat.a

SOURCES  := $(LIB_SRCS) test/tour/main.cpp test/driver/main.cpp
HEADERS  := $(wildcard include/*.h include/IR/*.h include/Support/*.h include/Pass/*.h include/Pass/Emit/*.h include/Pass/Opt/*.h include/CodeGen/*.h include/Target/*.h)
CC_FMT   := $(shell find test/cc -path test/cc/cases -prune -o \( -name '*.cpp' -o -name '*.h' \) -print | sort)

.PHONY: all run rat test test-c test-x86 compiledb format clean
all: compiledb bin/tour bin/rat

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(LIB_OBJS:.o=.d)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

bin/tour: test/tour/main.cpp $(LIB)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -o $@

run: bin/tour
	./bin/tour

bin/rat: test/driver/main.cpp $(LIB)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -o $@

rat: bin/rat

test:
	$(MAKE) -C test/cc test TESTBIN_ARGS="-j$$(nproc)"

test-c:
	$(MAKE) -C test/cc test-c TESTBIN_ARGS="-j$$(nproc)"

test-x86:
	$(MAKE) -C test/cc test-x86 TESTBIN_ARGS="-j$$(nproc)"

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
