# Host harness for the MicroPatterns C++ renderer core.
# Dependency-free: system clang/g++ and make, nothing else.
#
#   make            build build/mpharness
#   make verify     golden-image equivalence gate
#   make bench      benchmark the corpus
#   make bake       REGENERATE goldens -- read README.md first
#   make profile    build a -pg / frame-pointer flavour for profiling
#   make clean

CXX      ?= c++
FW_SRC   := ../../M5Paper_MicroPatterns/src
SHIM     := shim
SRC      := src
BUILD    := build

# HOST_LOG_LEVEL=0 keeps the shim's log_* macros silent so logging cannot
# distort benchmark timings. Override with `make LOGLEVEL=3`.
LOGLEVEL ?= 0

CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wno-unused-variable -Wno-sign-compare
CXXFLAGS += -I$(SHIM) -I$(SRC) -I$(FW_SRC) -DHOST_LOG_LEVEL=$(LOGLEVEL)
# -MMD -MP emits a .d file per object listing the headers it included, so a
# change to micropatterns_command.h (or any other header) rebuilds what depends
# on it. Without this the objects were only tracked against their .cpp, and a
# header-only change silently left stale objects in build/ -- which at best
# measures the wrong code and at worst links a binary that crashes.
CXXFLAGS += -MMD -MP

# Renderer core -- compiled VERBATIM from the firmware tree.
CORE_SRCS := \
  $(FW_SRC)/micropatterns_parser.cpp \
  $(FW_SRC)/micropatterns_runtime.cpp \
  $(FW_SRC)/micropatterns_drawing.cpp \
  $(FW_SRC)/display_list_renderer.cpp \
  $(FW_SRC)/occlusion_buffer.cpp \
  $(FW_SRC)/matrix_utils.cpp

HARNESS_SRCS := \
  $(SRC)/host_display_manager.cpp \
  $(SRC)/image_io.cpp \
  $(SRC)/render_path.cpp \
  $(SRC)/main.cpp

CORE_OBJS    := $(patsubst $(FW_SRC)/%.cpp,$(BUILD)/core_%.o,$(CORE_SRCS))
HARNESS_OBJS := $(patsubst $(SRC)/%.cpp,$(BUILD)/h_%.o,$(HARNESS_SRCS))
OBJS         := $(CORE_OBJS) $(HARNESS_OBJS)

BIN := $(BUILD)/mpharness

.PHONY: all clean verify bench bake list profile

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(BUILD)/core_%.o: $(FW_SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/h_%.o: $(SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

-include $(OBJS:.o=.d)

list: $(BIN)
	$(BIN) list --corpus corpus --golden golden

verify: $(BIN)
	$(BIN) verify --corpus corpus --golden golden

bench: $(BIN)
	$(BIN) bench --corpus corpus --reps 7 --json out/bench.json

bake: $(BIN)
	$(BIN) bake --corpus corpus --golden golden

# Frame pointers kept so `sample` / Instruments / xctrace produce readable
# stacks. See README.md "Profiling".
profile: CXXFLAGS += -fno-omit-frame-pointer
profile: clean all

clean:
	rm -rf $(BUILD)
