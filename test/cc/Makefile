# make        build bin/ratcc and bin/ratcc-test
# make test   build and run the golden test suite over all cases
# make run    tokenize each case (dump tokens) as a smoke test
# make format run clang-format over the frontend sources
# make clean  remove this frontend's build artifacts

ROOT     := ../..
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -I$(ROOT)/include -I.
DEPFLAGS := -MMD -MP

LIB      := $(ROOT)/build/rat.a
OBJDIR   := $(ROOT)/build/test/cc

CORE     := Lexer.cpp Parser.cpp Ast.cpp Emit.cpp EmitComplex.cpp EmitConst.cpp EmitType.cpp EmitInit.cpp EmitGlobal.cpp \
            Preprocess.cpp
CORE_OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(CORE))

HEADERS  := $(wildcard *.h)
SRCS     := $(CORE) main.cpp Runner.cpp

BIN      := $(ROOT)/bin/ratcc
TESTBIN  := $(ROOT)/bin/ratcc-test

CASES    := $(wildcard cases/custom/*.c) $(wildcard cases/c-testsuite/*.c) \
            $(wildcard cases/gcc-torture/*.c) $(wildcard cases/c99/*.c)
RAT_CASES := $(wildcard $(ROOT)/test/cases/*.rat)

.PHONY: all test run format clean
all: $(BIN) $(TESTBIN)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(patsubst %.cpp,$(OBJDIR)/%.d,$(SRCS))

.PHONY: $(LIB)
$(LIB):
	$(MAKE) -C $(ROOT) build/rat.a

$(BIN): $(CORE_OBJS) $(OBJDIR)/main.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(CORE_OBJS) $(OBJDIR)/main.o $(LIB) -o $@

$(TESTBIN): $(CORE_OBJS) $(OBJDIR)/Runner.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(CORE_OBJS) $(OBJDIR)/Runner.o $(LIB) -pthread -o $@

test: $(TESTBIN)
	@$(TESTBIN) $(TESTBIN_ARGS) $(CASES) $(RAT_CASES)

run: $(BIN)
	@for c in $(CASES); do echo "== $$c =="; $(BIN) -dump-tokens $$c; done

format:
	clang-format -i $(CORE) main.cpp Runner.cpp $(HEADERS)

clean:
	rm -rf $(OBJDIR) $(BIN) $(TESTBIN)
