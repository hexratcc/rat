# make          build bin/ratcc and bin/ratcc-test
# make test     build and run the golden test suite over all cases
# make test-c   run the suite through the C backend
# make test-x86 run the suite through the x86-64 backend
# make run      tokenize each case (dump tokens) as a smoke test
# make format   run clang-format over the frontend sources
# make clean    remove this frontend's build artifacts

ROOT     := ..
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -I$(ROOT)/rat/include -Iinclude
DEPFLAGS := -MMD -MP

LIB      := $(ROOT)/build/rat.a
OBJDIR   := $(ROOT)/build/cc

CORE     := $(shell find lib -name '*.cpp' | sort)
CORE_OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(CORE))

HEADERS  := $(shell find include -name '*.h' | sort)
SRCS     := $(CORE) main.cpp Runner.cpp

BIN      := $(ROOT)/bin/ratcc
TESTBIN  := $(ROOT)/bin/ratcc-test

CASES    := $(shell find $(ROOT)/cc/test -name '*.c' | sort)

.PHONY: all test test-c test-x86 run format clean
all: $(BIN) $(TESTBIN)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(patsubst %.cpp,$(OBJDIR)/%.d,$(SRCS))

.PHONY: $(LIB)
$(LIB):
	$(MAKE) -C $(ROOT)/rat lib

$(BIN): $(CORE_OBJS) $(OBJDIR)/main.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(CORE_OBJS) $(OBJDIR)/main.o $(LIB) -o $@

$(TESTBIN): $(CORE_OBJS) $(OBJDIR)/Runner.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(CORE_OBJS) $(OBJDIR)/Runner.o $(LIB) -pthread -o $@

test: $(TESTBIN)
	@$(TESTBIN) $(TESTBIN_ARGS) $(CASES)

test-c: $(TESTBIN)
	@RATCC_X86=0 $(TESTBIN) $(TESTBIN_ARGS) $(CASES)

test-x86: $(TESTBIN)
	@RATCC_X86=1 $(TESTBIN) $(TESTBIN_ARGS) $(CASES)

run: $(BIN)
	@for c in $(CASES); do echo "== $$c =="; $(BIN) -dump-tokens $$c; done

format:
	clang-format -i $(CORE) main.cpp Runner.cpp $(HEADERS)

clean:
	rm -rf $(OBJDIR) $(BIN) $(TESTBIN)
