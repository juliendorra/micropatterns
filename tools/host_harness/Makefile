# Host harness for the MicroPatterns C++ renderer core.
# Dependency-free: system clang/g++ and make, nothing else.
#
#   make            build build/mpharness
#   make verify     golden-image equivalence gate
#   make bench      benchmark the corpus
#   make bake       REGENERATE goldens -- read README.md first
#   make profile    build a -pg / frame-pointer flavour for profiling
#   make audit-js   web-renderer vs C++ goldens (needs node)
#   make cycle S=<script.mp>  does a script vary as $$COUNTER advances?
#   make sweep S=<script.mp>  which values can it reach across the clock?
#   make wasm       build the firmware renderer to WebAssembly (needs emsdk)
#   make verify-wasm  byte-compare the WASM build against the goldens
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

# mpsoak links the same core plus its own main. See src/soak.cpp for why the
# golden gate cannot cover what this covers.
SOAK_SRCS := $(SRC)/host_display_manager.cpp $(SRC)/soak.cpp

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

# ---------------------------------------------------------------------------
# Memory-safety gates. These build the SAME core sources into a separate
# directory with sanitizers on, so they never disturb build/ or a benchmark.
#
# SANITIZE  -fsanitize=address,undefined. A dangling read into the transform
#           snapshot pool, an out-of-range variable slot, or signed overflow in
#           expression evaluation becomes a hard failure with a stack trace,
#           instead of a slightly wrong pixel or nothing at all.
#
# STRICT    also -DMP_SHIM_NULL_CSTR=1, which makes the shim's String::c_str()
#           return NULL for an empty string. That is what the ESP32's WString
#           actually does (String::init sets buffer to nullptr), and it is a
#           fidelity gap that hides device-only null dereferences: any code that
#           does strcmp/strcasecmp on a possibly-empty String, or indexes
#           c_str()[0], is safe on a std::string-backed shim and crashes on
#           hardware. -O1 keeps sanitizer stacks readable.
# ---------------------------------------------------------------------------
SAN_FLAGS := -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
             -Wall -Wno-unused-variable -Wno-sign-compare \
             -I$(SHIM) -I$(SRC) -I$(FW_SRC) -DHOST_LOG_LEVEL=$(LOGLEVEL) -MMD -MP

.PHONY: sanitize verify-strict soak stress stress-clean

# Golden gate, rebuilt under sanitizers + the device-accurate NULL c_str().
verify-strict:
	$(MAKE) BUILD=build_strict CXXFLAGS="$(SAN_FLAGS) -DMP_SHIM_NULL_CSTR=1" build_strict/mpharness
	build_strict/mpharness verify --corpus corpus --golden golden

# Golden gate under sanitizers only (no shim behaviour change).
sanitize:
	$(MAKE) BUILD=build_san CXXFLAGS="$(SAN_FLAGS)" build_san/mpharness
	build_san/mpharness verify --corpus corpus --golden golden

$(BUILD)/mpsoak: $(patsubst $(SRC)/%.cpp,$(BUILD)/h_%.o,$(SOAK_SRCS)) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Lifetime / determinism soak. Renders the stress scripts through the shape the
# M5Paper firmware uses (one reused parser, runtime and renderer recreated per
# render) across many seeds, twice per seed, byte-comparing the two canvases.
# No goldens needed, so scripts can be added freely -- including generated ones.
soak: stress/generated
	$(MAKE) BUILD=build_soak CXXFLAGS="$(SAN_FLAGS) -DMP_SHIM_NULL_CSTR=1" build_soak/mpsoak
	build_soak/mpsoak --renders 24 stress/*.mp stress/generated/*.mp

stress/generated:
	python3 stress/generate.py --count 40 --out-dir stress/generated

stress: stress-clean stress/generated

stress-clean:
	rm -rf stress/generated

clean:
	rm -rf $(BUILD) build_san build_strict build_soak stress/generated

# --- web/device equivalence -------------------------------------------------
# Runs the EMULATOR's renderer headlessly (Node, no native canvas -- see
# js/render.mjs) over this same corpus and seeds, and byte-compares to the same
# goldens `verify` uses. This is the only check that the two implementations of
# the renderer actually agree.
.PHONY: audit-js audit-js-occlusion
audit-js:
	node js/render.mjs --compare-golden

# Does the WEB renderer's own occlusion culling change its output? It must not.
audit-js-occlusion:
	node js/render.mjs --compare-occlusion

# --- WebAssembly build of the firmware renderer -----------------------------
# The same six core .cpp files this Makefile compiles for native, compiled by
# Emscripten instead. Not a port: the core is already free of Arduino, FreeRTOS
# and hardware, which is what makes both builds possible from one source.
#
# Needs an emsdk; wasm/build.sh explains where it looks. Output is gitignored --
# it is reproducible, and deploying it is a separate decision.
.PHONY: wasm verify-wasm
wasm:
	./wasm/build.sh

verify-wasm: wasm
	node wasm/verify.mjs

# --- variation, not equality ------------------------------------------------
# Every other gate here compares single frames. None of them can see a script
# that renders byte-perfect frames and still repeats itself every 32 draws.
#   make cycle S=corpus/city.mp
cycle:
	@test -n "$(S)" || { echo "usage: make cycle S=<script.mp> [DRAWS=64]"; exit 2; }
	./build/mpharness cycle $(S) --draws $(or $(DRAWS),64)

# Which values can a script REACH? `cycle` catches repeats; this catches a
# script that never repeats and still only reaches six of its ten variations.
# The script must encode one integer as its non-white pixel count.
#   make sweep S=probe/corpus/sweep_32749.mp LEVELS=10
sweep:
	@test -n "$(S)" || { echo "usage: make sweep S=<script.mp> [LEVELS=10]"; exit 2; }
	./build/mpharness sweep $(S) --levels $(or $(LEVELS),10) --width 200 --height 200
