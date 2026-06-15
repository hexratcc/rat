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
	lib/Pass/Pass.cpp \
	lib/Pass/PassManager.cpp \
	lib/Pass/PrintPass.cpp

LIB_OBJS := $(patsubst %.cpp,build/%.o,$(LIB_SRCS))
LIB      := build/libratson.a

SOURCES  := $(LIB_SRCS) test/tour/main.cpp
HEADERS  := $(wildcard include/*.h include/IR/*.h include/Pass/*.h)

.PHONY: all run format clean
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

format:
	clang-format -i $(SOURCES) $(HEADERS)

clean:
	rm -rf build bin
