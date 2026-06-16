#  make        build bin/tour
#  make run    build and run the tour example
#  make format run clang-format over the sources
#  make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -Iinclude
DEPFLAGS := -MMD -MP

LIB_SRCS := \
	lib/IR/Opcode.cpp \
	lib/IR/Type.cpp \
	lib/IR/Node.cpp \
	lib/IR/Function.cpp \
	lib/IR/Module.cpp \
	lib/IR/TextParser.cpp \
	lib/Pass/Pass.cpp \
	lib/Pass/PassManager.cpp \
	lib/Pass/Verify.cpp \
	lib/Pass/Emit/TextEmitter.cpp \
	lib/Pass/Emit/GraphEmitter.cpp \
	lib/Pass/Emit/CEmitter.cpp \
	lib/Pass/Opt/Fold.cpp \
	lib/Pass/Opt/GVN.cpp \
	lib/Pass/Opt/SimplifyCFG.cpp \
	lib/Pass/Opt/AliasAnalysis.cpp \
	lib/Pass/Opt/MemoryOpt.cpp \
	lib/CodeGen/Schedule.cpp

LIB_OBJS := $(patsubst %.cpp,build/%.o,$(LIB_SRCS))
LIB      := build/rat.a

SOURCES  := $(LIB_SRCS) test/tour/main.cpp test/runner/main.cpp
HEADERS  := $(wildcard include/*.h include/IR/*.h include/Support/*.h include/Pass/*.h include/Pass/Emit/*.h include/Pass/Opt/*.h include/CodeGen/*.h)

CASES := $(wildcard test/cases/*.rat)

.PHONY: all run test format clean
all: bin/tour

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

bin/ratest: test/runner/main.cpp $(LIB)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -o $@

test: bin/ratest
	./bin/ratest $(CASES)

format:
	clang-format -i $(SOURCES) $(HEADERS)

clean:
	rm -rf build bin
