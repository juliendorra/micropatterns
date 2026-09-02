# Host harness for the MicroPatterns C++ renderer core.
# Dependency-free: system clang/g++ and make, nothing else.
#
#   make            build build/mpharness
#   make verify     golden-image equivalence gate
#   make bench      benchmark the corpus
#   make bake       REGENERATE goldens -- read README.md first
#   make profile    build a -pg / frame-pointer flavour for profiling
#   make cycle S=<script.mp>  does a script vary as $$COUNTER advances?
#   make sweep S=<script.mp>  which values can it reach across the clock?
#   make audit-sweep  the sweep gate, including the cases that must FAIL
#   make wasm       build the firmware renderer to WebAssembly (needs emsdk)
#   make verify-wasm  byte-compare the WASM build against the goldens
#   make ci          complete required engine/emulator drift gate
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
  $(FW_SRC)/mp_program.cpp \
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
# STRICT    also -DMP_SHIM_NULL_CSTR=1, an aggressive null-safety stress mode.
#           Exact WString is used by constrained WASM; this std::string shim
#           deliberately returns NULL for every empty string to expose callers
#           that assume c_str() is always valid. -O1 keeps sanitizer stacks
#           readable.
# ---------------------------------------------------------------------------
SAN_FLAGS := -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
             -Wall -Wno-unused-variable -Wno-sign-compare \
             -I$(SHIM) -I$(SRC) -I$(FW_SRC) -DHOST_LOG_LEVEL=$(LOGLEVEL) -MMD -MP

.PHONY: sanitize verify-strict soak stress stress-clean

# Golden gate, rebuilt under sanitizers + aggressive NULL-c_str() stress.
verify-strict:
	$(MAKE) BUILD=build_strict CXXFLAGS="$(SAN_FLAGS) -DMP_SHIM_NULL_CSTR=1" build_strict/mpharness
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build_strict/mpharness verify --corpus corpus --golden golden

# Golden gate under sanitizers only (no shim behaviour change).
sanitize:
	$(MAKE) BUILD=build_san CXXFLAGS="$(SAN_FLAGS)" build_san/mpharness
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build_san/mpharness verify --corpus corpus --golden golden

$(BUILD)/mpsoak: $(patsubst $(SRC)/%.cpp,$(BUILD)/h_%.o,$(SOAK_SRCS)) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Lifetime / determinism soak. Renders the stress scripts through the shape the
# M5Paper firmware uses (one reused parser, runtime and renderer recreated per
# render) across many seeds, twice per seed, byte-comparing the two canvases.
# No goldens needed, so scripts can be added freely -- including generated ones.
soak: stress/generated
	$(MAKE) BUILD=build_soak CXXFLAGS="$(SAN_FLAGS) -DMP_SHIM_NULL_CSTR=1" build_soak/mpsoak
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build_soak/mpsoak --renders 24 stress/*.mp stress/generated/*.mp

stress/generated:
	python3 stress/generate.py --count 40 --out-dir stress/generated

stress: stress-clean stress/generated

stress-clean:
	rm -rf stress/generated

clean:
	rm -rf $(BUILD) build_san build_strict build_soak stress/generated

# --- WebAssembly build of the firmware renderer -----------------------------
# The same six core .cpp files this Makefile compiles for native, compiled by
# Emscripten instead. Not a port: the core is already free of Arduino, FreeRTOS
# and hardware, which is what makes both builds possible from one source.
#
# Needs an emsdk; wasm/build.sh explains where it looks. Output is gitignored --
# it is reproducible, and deploying it is a separate decision.
.PHONY: wasm verify-wasm verify-language-wasm verify-device-diagnostics wasm-constrained \
	verify-wasm-constrained verify-source-parity verify-emulator-wasm \
	sync-wasm sync-wasm-constrained ci
wasm:
	./wasm/build.sh

verify-wasm: wasm
	node wasm/verify.mjs

verify-language-wasm: wasm
	node wasm/verify_language.mjs

verify-device-diagnostics:
	node wasm/verify_device_diagnostics.mjs

wasm-constrained:
	./wasm/build_constrained.sh

verify-wasm-constrained: wasm wasm-constrained
	node wasm/verify_constrained.mjs

sync-wasm-constrained:
	./wasm/sync_constrained_to_emulator.sh

sync-wasm:
	./wasm/sync_to_emulator.sh

verify-source-parity:
	node ../ci/verify-renderer-source-parity.mjs

# The online editor commits generated files because FTP deployment has no build
# step. Compare bytes, not timestamps or git state, so this also catches absent
# and untracked artifacts in a fresh checkout.
verify-emulator-wasm: wasm wasm-constrained
	@for file in mp_render.js mp_render.wasm \
		mp_render_watchy.js mp_render_watchy.wasm \
		mp_render_m5paper.js mp_render_m5paper.wasm; do \
		cmp "wasm/out/$$file" "../../micropatterns_emulator/wasm/$$file" || { \
			echo "STALE emulator WASM artifact: $$file" >&2; \
			exit 1; \
		}; \
	done
	@echo "PASS  online-editor WASM artifacts match fresh builds"

# Required pre-deploy gate. The longer generated lifetime soak remains a
# scheduled/manual CI job; these checks are deterministic and run on every push.
ci: verify audit-sweep sanitize verify-strict verify-source-parity \
	verify-wasm verify-language-wasm verify-device-diagnostics \
	verify-wasm-constrained verify-emulator-wasm

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

# The sweep gate. Two cases must FAIL, so expectations are declared per case --
# a negative test is only a test if something checks it still goes negative.
# Also verifies the city.mp probes are still verbatim copies of their source.
audit-sweep: $(BIN)
	./probe/audit_sweep.sh
