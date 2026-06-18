# make        build bin/ratcc
# make run    tokenize cases/*.c (dump tokens) as a smoke test
# make format run clang-format over the frontend sources
# make clean  remove this frontend's build artifacts

ROOT     := ../..
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INC      := -I$(ROOT)/include -I.
DEPFLAGS := -MMD -MP

LIB      := $(ROOT)/build/rat.a

SRCS     := $(wildcard *.cpp)
OBJS     := $(patsubst %.cpp,$(ROOT)/build/test/cc/%.o,$(SRCS))
HEADERS  := $(wildcard *.h)
BIN      := $(ROOT)/bin/ratcc

CASES    := $(wildcard cases/*.c)

.PHONY: all run format clean
all: $(BIN)

$(ROOT)/build/test/cc/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

-include $(OBJS:.o=.d)

.PHONY: $(LIB)
$(LIB):
	$(MAKE) -C $(ROOT) build/rat.a

$(BIN): $(OBJS) $(LIB)
	@mkdir -p $(ROOT)/bin
	$(CXX) $(CXXFLAGS) $(INC) $(OBJS) $(LIB) -o $@

run: $(BIN)
	@for c in $(CASES); do echo "== $$c =="; $(BIN) -dump-tokens $$c; done

format:
	clang-format -i $(SRCS) $(HEADERS)

clean:
	rm -rf $(ROOT)/build/test/cc $(BIN)
