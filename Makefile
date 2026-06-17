# make           build bin/tour and bin/rat
# make run       build and run the tour example
# make rat       build the bin/rat pipeline driver
# make test      run tests
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

SOURCES  := $(LIB_SRCS) test/tour/main.cpp test/runner/main.cpp test/driver/main.cpp
HEADERS  := $(wildcard include/*.h include/IR/*.h include/Support/*.h include/Pass/*.h include/Pass/Emit/*.h include/Pass/Opt/*.h include/CodeGen/*.h)

CASES := $(wildcard test/cases/*.rat)

.PHONY: all run rat test compiledb format clean
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

bin/ratest: test/runner/main.cpp $(LIB)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -o $@

test: bin/ratest
	./bin/ratest $(CASES)

compiledb:
	@printf '[\n' > compile_commands.json
	@first=1; for f in $(SOURCES); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; first=0; \
		printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s %s -c %s"\n  }' \
			"$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$(INC)" "$$f" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json

format:
	clang-format -i $(SOURCES) $(HEADERS)

clean:
	rm -rf build bin
