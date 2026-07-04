# make          build bin/rat and bin/rat-test
# make test     build and run the rat IR golden suite
# make format   run clang-format over the rat test sources
# make clean    remove this suite's build artifacts

ROOT     := ../..
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -I$(ROOT)/rat/include
DEPFLAGS := -MMD -MP

LIB      := $(ROOT)/build/rat.a
OBJDIR   := $(ROOT)/build/rat/test

SRCS     := driver.cpp Runner.cpp

BIN      := $(ROOT)/bin/rat
TESTBIN  := $(ROOT)/bin/rat-test

CASES    := $(wildcard $(ROOT)/rat/test/*.rat)

.PHONY: all test format clean
all: $(BIN) $(TESTBIN)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(patsubst %.cpp,$(OBJDIR)/%.d,$(SRCS))

.PHONY: $(LIB)
$(LIB):
	$(MAKE) -C $(ROOT) build/rat.a

$(BIN): $(OBJDIR)/driver.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(OBJDIR)/driver.o $(LIB) -o $@

$(TESTBIN): $(OBJDIR)/Runner.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(OBJDIR)/Runner.o $(LIB) -pthread -o $@

test: $(TESTBIN)
	@$(TESTBIN) $(TESTBIN_ARGS) $(CASES)

format:
	clang-format -i $(SRCS)

clean:
	rm -rf $(OBJDIR) $(BIN) $(TESTBIN)
