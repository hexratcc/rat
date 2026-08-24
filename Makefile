MODE ?= release
CXX ?= c++

ifeq ($(MODE),release)
OPT := -O2
else ifeq ($(MODE),debug)
OPT := -O0 -g
else
$(error MODE must be release or debug)
endif

FLAGS := -std=c++17 -Wall -Wextra -pthread $(OPT)
OBJ := build/$(MODE)
INC := -iquote src/base -iquote src/backend -iquote src/linker -iquote src/compiler -iquote $(OBJ)

# source groups
base_srcs := src/base/test_harness.cpp
rat_srcs := $(shell find src/backend/analysis src/backend/codegen src/backend/ir \
	src/backend/pass src/backend/target -name '*.cpp')
link_srcs := src/linker/linker.cpp src/linker/elf_write.cpp src/linker/elf_read.cpp
cc_srcs := $(filter-out src/compiler/main.cpp,$(wildcard src/compiler/*.cpp)) \
	$(shell find src/compiler/emit src/compiler/lex src/compiler/parse -name '*.cpp')

objs = $(patsubst %.cpp,$(OBJ)/%.o,$(1))
base_o := $(call objs,$(base_srcs))
rat_o := $(call objs,$(rat_srcs))
link_o := $(call objs,$(link_srcs))
cc_o := $(call objs,$(cc_srcs))
driver_o = $(OBJ)/src/$(1).o
all_o := $(base_o) $(rat_o) $(link_o) $(cc_o) \
	$(call driver_o,backend/main) $(call driver_o,backend/test/runner) \
	$(call driver_o,linker/main) $(call driver_o,compiler/main) \
	$(call driver_o,compiler/test/correctness/runner)

all: bin/rat bin/rat-test bin/link bin/cc bin/cc-test

$(OBJ)/git_hash.h: FORCE
	@mkdir -p $(OBJ)
	@h=$$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown); \
	if ! git diff --quiet HEAD -- 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then \
		h="$$h-dirty"; \
	fi; \
	new="#define GIT_HASH \"$$h\""; \
	{ [ -f $@ ] && [ "$$(cat $@)" = "$$new" ]; } || echo "$$new" > $@

$(OBJ)/%.o: %.cpp | $(OBJ)/git_hash.h
	@mkdir -p $(dir $@)
	@echo "cc   $<"
	@$(CXX) $(FLAGS) $(INC) -MMD -MP -c $< -o $@

-include $(all_o:.o=.d)

bin/%:
	@mkdir -p bin
	@echo "link $@"
	@$(CXX) $(FLAGS) -o $@ $^

bin/rat: $(base_o) $(rat_o) $(call driver_o,backend/main)
bin/rat-test: $(base_o) $(rat_o) $(call driver_o,backend/test/runner)
bin/link: $(base_o) $(link_o) $(call driver_o,linker/main)
bin/cc: $(base_o) $(rat_o) $(cc_o) $(call driver_o,compiler/main)
bin/cc-test: $(base_o) $(rat_o) $(cc_o) $(link_o) $(call driver_o,compiler/test/correctness/runner)

test: all
	./src/compiler/test/run.py $(SUITES)

bench: all
	./src/compiler/test/run.py bench $(SUITES)

clean:
	rm -rf build bin

.PHONY: all test bench clean FORCE
