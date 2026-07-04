# make          build the core lib, bin/rat driver, and bin/rat-test harness
# make lib      build only the core library (build/rat.a)
# make test     build and run the rat IR golden suite
# make format   run clang-format over the rat sources
# make clean    remove this project's build artifacts

ROOT     := ..
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -Iinclude
DEPFLAGS := -MMD -MP

OBJDIR   := $(ROOT)/build/rat

LIB_SRCS := $(shell find lib -name '*.cpp' | sort)
LIB_OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SRCS))
LIB      := $(ROOT)/build/rat.a

DRIVER   := test/driver.cpp
HARNESS  := test/Runner.cpp
HEADERS  := $(shell find include -name '*.h' | sort)

BIN      := $(ROOT)/bin/rat
TESTBIN  := $(ROOT)/bin/rat-test

CASES    := $(wildcard test/*.rat)

.PHONY: all lib test format clean
all: $(BIN) $(TESTBIN)

lib: $(LIB)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(LIB_OBJS:.o=.d)
-include $(OBJDIR)/test/driver.d $(OBJDIR)/test/Runner.d

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BIN): $(OBJDIR)/test/driver.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -o $@

$(TESTBIN): $(OBJDIR)/test/Runner.o $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(LIB) -pthread -o $@

test: $(TESTBIN)
	@$(TESTBIN) $(TESTBIN_ARGS) $(CASES)

format:
	clang-format -i $(LIB_SRCS) $(DRIVER) $(HARNESS) $(HEADERS)

clean:
	rm -rf $(OBJDIR) $(LIB) $(BIN) $(TESTBIN)
