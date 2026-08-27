# M5Paper front-half performance analysis (parsing, interpretation, display-list generation)

Scope: `micropatterns_parser.{cpp,h}`, `micropatterns_runtime.{cpp,h}`, `micropatterns_command.h`,
`script_manager.cpp`, and comparison against the JS reference (`micropatterns_emulator/`).
The rasterizer (`display_list_renderer.cpp`) is explicitly out of scope — covered by a separate analysis.

All timing numbers below that are not directly quoted from code comments/logs are **estimates**, labeled as such.

---

## 1. What is (and isn't) actually measured

`render_controller.cpp` (`RenderController::renderScript`) has two `millis()` brackets:

```cpp
unsigned long generationStartTime = millis();
_runtime->generateDisplayList();
unsigned long generationDuration = millis() - generationStartTime;
...
log_i("... Display list generation for '%s' took %lu ms. List size: %d", ...);

unsigned long renderStartTime = millis();
_renderer->render(_runtime->getDisplayList());
unsigned long renderDuration = millis() - renderStartTime;
...
log_i("... Display list rendering for '%s' took %lu ms.", ...);
```

So we **do** get a genuine split between "interpretation + display-list generation" and "rasterization",
logged at `log_i` on every render. That's real, existing signal — worth mining device logs for before
guessing further.

What is **not** measured, anywhere in the front half:

- **Parsing time.** `_parser.parse(script_content)` at line 42 of `render_controller.cpp` has no
  `millis()` bracket at all. For scripts with many `DEFINE PATTERN` blocks (each with up to hundreds of
  characters of `DATA="010101..."` that get parsed char-by-char into a `std::vector<uint8_t>`) and many
  lines, parse cost is currently invisible. Given parsing runs once per script load/edit (not once per
  REPEAT iteration), it is almost certainly much smaller than display-list generation for
  loop-heavy scripts, but this is an assumption, not a measurement — it should be bracketed with
  `millis()` immediately, it's a 2-line change.
- **JSON deserialization / S3 fetch time** in `script_manager.cpp` / `network_manager.cpp` (see §6).
- **Any sub-phase inside `generateDisplayList()`** — e.g. how much of the reported "generation" time is
  spent in `std::map<String,...>` lookups vs `evaluateExpression` vs `String` allocation vs pushing
  `DisplayListItem`s into the vector. Nothing internal is instrumented.

**Bottom line finding:** we know the interpretation/generation-vs-rasterization split (it's logged), but
we do NOT know the parse-vs-interpret split, nor have any internal profiling of `generateDisplayList()`
itself. Anyone citing "parsing is fast, it's all in the loop" is inferring, not measuring. The single
highest-leverage non-invasive next step, before writing any optimization code, is to (a) collect several
`log_i` traces from a real device across a range of real scripts (simple vs. `city.json`-style
nested-REPEAT scripts) to see how generation-time scales with display-list size, and (b) add one more
`millis()` bracket around `_parser.parse()`.

---

## 2. Interpreter data structures: the embedded-hostile inventory

### 2.1 `std::list<MicroPatternsCommand>` for the command stream

`micropatterns_command.h`:
```cpp
std::list<MicroPatternsCommand> nestedCommands; // REPEAT body
std::list<MicroPatternsCommand> thenCommands;   // IF body
std::list<MicroPatternsCommand> elseCommands;
```
and the top-level `MicroPatternsParser::_commands` is also a `std::list`.

Each `MicroPatternsCommand` (see §2.4) is a heavyweight object (multiple `std::map`, multiple
`std::vector<ParamValue>`, several `String`s). A `std::list` node individually heap-allocates each
element (typically `malloc` for the node header + payload), so:

- No cache locality: walking the command list for each REPEAT iteration chases pointers scattered across
  the heap (worse: ESP32 PSRAM has materially higher access latency than internal SRAM, and `std::list`'s
  pointer-chasing pattern is close to worst-case for PSRAM).
- The command list for the loop body is walked **N times** for an N-iteration REPEAT (see
  `processCommandForDisplayList`, `CMD_REPEAT` case, `for (int i = 0; i < count; ++i) { for (const auto&
  nestedCmd : cmd.nestedCommands) ... }`) — the iterator/pointer-chase cost is paid once per iteration,
  not once total.
- `std::list` fragments the heap on a device with only 4MB PSRAM and no MMU-backed compaction; repeated
  script loads (edits) leave old fragments behind until freed, and ESP32's heap allocator is not
  particularly fragmentation-resistant.

This is a static, per-parse-time-built structure — it's built once and then read many times during
generation — so it's a good candidate for compaction into a `std::vector` (contiguous) since it never
needs insertion after parsing completes.

### 2.2 `std::map<String, ParamValue> params` per command, walked by *string key* every access

Every `resolveIntParam`/`resolveStringParam`/`resolveAssetNameParam` call does:
```cpp
String upperParamName = paramName;   // heap alloc + copy
upperParamName.toUpperCase();        // in-place, but paramName was just copied
if (params.count(upperParamName)) {  // std::map<String,...> lookup: O(log n) String comparisons
    const ParamValue& val = params.at(upperParamName); // second O(log n) lookup (redundant with count())
```
Two full `std::map<String,ParamValue>` lookups (each doing `strcmp`-style comparisons at every tree node,
`std::map` is a red-black tree) **per parameter, per command execution**. For a command like `DRAW` with
2 int params + 1 string param, that's already 3 params × 2 lookups × (Arduino `String` comparison cost) =
6 map traversals, each doing several `String::compareTo`-equivalent calls, for every single DRAW
executed — and DRAW is exactly the command that appears inside hot REPEAT loops.

Arduino `String` comparison is not a cheap `memcmp` on a length-prefixed buffer in the fast path the way a
small-string-optimized `std::string` might be — it's a full C-string-style walk, and every temporary
(`upperParamName`) is a **heap allocation** (`String` has no SSO on the ESP32 Arduino core; anything
beyond construction from a literal typically touches the heap, and `toUpperCase()`/`+`/copy-construction
each allocate).

### 2.3 `std::map<String,int> _variables` / `_environment`, `std::set<String> _declaredVariables`

Same shape of problem as §2.2 but for every `$VAR` read/write:
```cpp
int MicroPatternsRuntime::resolveValue(const ParamValue& val, int lineNumber, int loopIndex) {
    ...
    String varName = val.stringValue;   // COPY (heap alloc)
    varName.toUpperCase();              // mutate copy
    if (varName == "$INDEX") { ... }    // String == const char* comparison
    if (_environment.count(varName)) return _environment.at(varName); // map lookup #1 (+ #2 via .at)
    if (_variables.count(varName)) return _variables.at(varName);     // map lookup #1 (+ #2 via .at)
    ...
}
```
Every single `$VAR` reference in an expression allocates a `String` copy, uppercases it, does a literal
string compare against `"$INDEX"`, then up to 4 red-black-tree traversals (`count`+`at` against
`_environment`, then possibly again against `_variables`) before it resolves to an `int`. `LET` writes go
through `"$" + cmd.letTargetVar` (another heap allocation via `String::operator+`) then a map lookup.

`_declaredVariables` (a `std::set<String>`, populated by the parser) isn't even consulted by the runtime
at generation time (only used for `LET` validation implicitly via `_variables.count()` — actually LET
checks `_variables.count(targetVarKey)`, not `_declaredVariables` — so the set is parse-time-only
machinery, cheap in comparison; not a hot-path contributor).

### 2.4 Arduino `String` allocation count — a worked example

Consider the actual `city.json` example (`micropatterns_server/local-s3-storage/scripts/7qpkkx4dys/city.json`):
a nested REPEAT (`grid_height` × `grid_width` iterations — with `$scaling=4` and a 960×540 canvas this
computes to roughly 108 × 192 ≈ 20,000 outer×inner iterations, though the script's own grid-size formula
multiplying by `$scaling` looks like it may be an authoring bug producing an oversized grid — either way
it's representative of "many thousands of loop bodies"). Each inner-loop body contains, per iteration:

- 10 `VAR` declarations + several `LET`s (each is one expression evaluation → each `$VAR` token in the
  expression triggers the §2.3 String-copy + map-lookup chain)
- 8 `IF` blocks, each evaluating a condition (2 more expression evaluations, more `$VAR` resolutions)
- 1 `COLOR`, 1 `SCALE`, up to 8 conditional `DRAW`s (only one fires), 1 `RESET_TRANSFORMS`

Rough per-iteration tally (order-of-magnitude, **estimated**, not measured):
- ~25–35 `$VAR` resolutions → ~25–35 `String` copies (in `resolveValue`) + ~50–70 map lookups (`count`+`at`
  pairs against `_environment`/`_variables`)
- ~10 `resolveIntParam`/`resolveStringParam` calls for the DRAW/FILL_RECT/etc. params actually reached →
  another ~10 `String` param-name-uppercase copies + ~20 map lookups against `cmd.params`
- Several `String` concatenations (`"$" + cmd.varName`, `"$" + cmd.letTargetVar`) for VAR/LET targets

That's on the order of **40–50 Arduino `String` heap allocations and 70–100 `std::map` red-black-tree
traversals per loop iteration**, multiplied by ~20,000 iterations for this one script ⇒ **roughly
800,000–1,000,000 String allocations and 1.5–2 million map traversal operations** just to generate the
display list for a single render of this script. Each `String` heap alloc/free pair on ESP32's default
allocator is not free (it's not just a bump allocator — it involves the heap's free-list bookkeeping,
compaction avoidance heuristics, and on this SoC, if PSRAM is involved, external-memory latency). This
volume of small, short-lived heap churn is a highly plausible dominant cost for the "10–19 seconds",
though — per §1 — it is *not directly measured*, so treat the magnitude as illustrative rather than proven.

---

## 3. The compiled-form fix, and the JS-side prior art

### 3.1 What the JS side already has that C++ does not

The JS emulator (`micropatterns_emulator/`) contains **two parallel execution pipelines**:

1. `parser.js` + `display_list_generator.js` (`DisplayListGenerator`) — this is the architectural
   sibling of the C++ `micropatterns_parser.cpp` + `micropatterns_runtime.cpp`: a tree-walking
   interpreter over parsed commands, using plain JS objects/`Map`s for variables, producing a display
   list consumed by `display_list_renderer.js`. **This is the pipeline the C++ code mirrors.**
2. `compiler.js` (1656 lines) + `compiled_runtime.js` (785 lines) — a **second, more aggressive
   pipeline** (`MicroPatternsCompiler` → `MicroPatternsCompiledRunner`) that is wired up in
   `simulator.js` alongside the first. This compiler does source-to-function compilation with an
   explicit optimization pass list (`optimizationConfig` in `compiler.js`):
   - `enableConstantFolding` — fold constant expressions at compile time
   - `enableInvariantHoisting` — hoist loop-invariant calculations out of REPEAT bodies
   - `enableLoopUnrolling` (`loopUnrollThreshold: 8`) — unroll small constant-count loops
   - `enableTransformCaching` — cache transformation matrices (`this.transformCache = new Map()`)
   - `enablePatternTileCaching` — cache pre-rendered pattern tiles
   - `enablePixelBatching` / `enableDrawCallBatching` — batch primitive draw calls
   - `enableDeadCodeElimination`, `enableTransformSequencing`, `enableDrawOrderOptimization`,
     `enableMemoryOptimization`
   - `_analyzeScriptForOptimizations()` walks the command tree once, up front, to find REPEAT loops,
     common transform sequences, common pattern fills, and loop invariants — i.e. exactly the kind of
     one-time static analysis a "compile once" step should do.

> **[CORRECTED BY PROJECT OWNER — 2026-08-27]** The paragraph that stood here claimed the
> compiler pipeline was the advanced path and that the C++ runtime was "stuck at the unoptimized
> `display_list_generator.js`". **That is backwards, and it was wrong.** See §3.1b below for the
> corrected account. The original claim is struck rather than deleted so the mistake stays on the
> record.

### 3.1b The compiler path is the SUPERSEDED path, not the advanced one

The three JS execution paths are not a progression from naive to advanced. They are **competing
implementations that were benchmarked against each other**, and the display list won.

Git history establishes the order unambiguously:

| Commit | Meaning |
|---|---|
| `45d6a66` compute optimizations | early ad hoc optimization work |
| `e194dd0` Optimization ported to javascript web version | |
| `6d685c6` / `0843193` compiler and profiling / Improve compiler and profiling | the compiler path is built |
| `d1ea9e7` **Compiler is default** | the compiler wins, for a while |
| `82906d5` first version of a display list with reverse order painting | the display list path is built |
| `01ded93` / `e4d18d9` Improved display list, more precise culling / Correct display list transforms and cache key | display list matures |
| `d427b02` **All path gives same result** | equivalence established across paths — the precondition for a fair benchmark |
| `68d843e` **Default to display list** | the display list wins and replaces the compiler as default |
| `20cf696` / `d8d5e76` clean up optimization choices / clean up optimization setting | consolidation around the winner |
| `27eda12` Render use display list with pixel occupancy map | the C++ firmware follows the winner |

The project owner confirms directly: **"DisplayList was faster in tests."**

`micropatterns_emulator/index.html:103` still reflects this — `<input type="radio"
name="executionPath" value="displayList" checked>`. The compiler path remains selectable in the UI as
a comparison baseline, not as the recommended path.

**What this means for the C++ firmware.** The C++ runtime mirroring `display_list_generator.js` is a
*deliberate, benchmark-backed choice*, not technical debt and not a parity gap. There is no
"advanced JS pipeline the device is missing." Any proposal in this document framed as "port
`compiler.js` to C++" is therefore proposing to port **the path that already lost**, and must be
re-justified from scratch rather than treated as catching up.

**What survives the correction.** The compiler's individual *techniques* — constant folding,
loop-invariant hoisting, transform/pattern caching — are not invalidated by the pipeline losing;
they were simply attached to a pipeline that lost for other reasons (most plausibly that the display
list's culling and reverse-order painting beat the compiler's per-command savings). Those techniques
could still be applied *inside* the display-list generator. That is a genuinely open idea. But it
must be argued on its own merits and measured, not smuggled in under a false "the JS side already
does this" premise.

**Why this error happened, recorded as a methodological lesson.** The analysis inferred maturity
from code sophistication and file size (`compiler.js` is 1656 lines, `display_list_generator.js` is
455) and never checked `git log` ordering or the default in the UI. Bigger and cleverer was assumed
to mean newer and better. In a repo that keeps losing experiments alive as selectable options, that
inference is unsound. **Check which option is the default, and check the commit order, before
declaring anything "the advanced path."**

### 3.2 Proposed C++ structural fix: parse-time "compile" pass

Concretely, for the C++ firmware:

1. **Flatten command storage.** Replace `std::list<MicroPatternsCommand>` (top-level and nested) with
   `std::vector<MicroPatternsCommand>` plus explicit jump offsets for REPEAT/IF blocks (a flat
   instruction stream with `REPEAT_START`/`REPEAT_END`/`JUMP_IF_FALSE`-style markers, like a tiny
   bytecode), so `generateDisplayList()` walks a contiguous array with an index, not a linked list with
   iterator/pointer chasing, and loop bodies don't need recursive `std::list` iteration per outer
   iteration.
2. **Intern variable and parameter names to integer slots at parse time.** The parser already validates
   variable names (`validateVariableUsage`, `_declaredVariables`) and knows every `$VAR` reference in the
   script up front. Assign each unique variable name a small integer index during parsing, and store
   `ParamValue`/expression tokens as `TYPE_VAR_SLOT` + `int slotIndex` instead of `TYPE_VARIABLE` +
   `String stringValue`. At runtime, `_variables` becomes a flat `std::vector<int>` (or
   fixed-size `int[MAX_VARS]`) indexed directly — O(1), no `String`, no map, no allocation. Same for
   `_environment` (`$WIDTH`,`$HEIGHT`,`$HOUR`,`$MINUTE`,`$SECOND`,`$COUNTER`,`$INDEX` are a fixed, known
   set — these can be plain named `int` fields or a tiny fixed-size array indexed by an enum, not a map
   at all).
3. **Intern/enum-ize parameter names.** Command params (`X`,`Y`,`WIDTH`,`HEIGHT`,`RADIUS`,`X1`,`Y1`,`X2`,
   `Y2`,`DX`,`DY`,`DEGREES`,`FACTOR`,`NAME`) are a small closed set known at parse time per command type.
   Replace `std::map<String, ParamValue> params` with a small `struct` per command type (a tagged union /
   variant keyed by `CommandType`) with named fields, or at minimum a fixed-size
   `std::array<ParamValue, MAX_PARAMS_PER_CMD>` indexed by a `ParamSlot` enum. This removes essentially
   all of the `resolveIntParam`/`resolveStringParam` `String`-uppercasing + map-lookup overhead described
   in §2.2.
4. **Do the "compile" work once, at parse/load time, not on every wake/render.** Given `$COUNTER`/`$TIME`
   are the only things that legitimately change between wakes (see §6), the slot-resolution pass, dead
   command pruning, constant folding, etc. can all run once when a script is loaded/edited rather than on
   every `generateDisplayList()` call.

### 3.3 Code sketch — slot-indexed variables (the highest-value single change)

```cpp
// micropatterns_command.h — extend ParamValue
struct ParamValue {
    enum ValueType { TYPE_INT, TYPE_STRING, TYPE_VAR_SLOT, TYPE_ENV_SLOT, TYPE_OPERATOR } type;
    int intValue;      // literal int, OR slot index when type is TYPE_VAR_SLOT/TYPE_ENV_SLOT
    String stringValue; // only populated for TYPE_STRING / TYPE_OPERATOR now
};

// micropatterns_runtime.h
enum EnvSlot { ENV_WIDTH, ENV_HEIGHT, ENV_HOUR, ENV_MINUTE, ENV_SECOND, ENV_COUNTER, ENV_INDEX, ENV_SLOT_COUNT };
int _envValues[ENV_SLOT_COUNT];
std::vector<int> _varValues; // sized to declaredVariables.size() after parse

// micropatterns_runtime.cpp
int MicroPatternsRuntime::resolveValue(const ParamValue& val, int lineNumber, int loopIndex) {
    switch (val.type) {
        case ParamValue::TYPE_INT: return val.intValue;
        case ParamValue::TYPE_ENV_SLOT:
            if (val.intValue == ENV_INDEX) {
                if (loopIndex < 0) { runtimeError("...", lineNumber); return 0; }
                return loopIndex;
            }
            return _envValues[val.intValue];
        case ParamValue::TYPE_VAR_SLOT:
            return _varValues[val.intValue]; // O(1), no alloc, no compare
        default:
            runtimeError("...", lineNumber);
            return 0;
    }
}
```
The parser resolves `"$FOO"` → slot index once, at parse time, in `parseExpression`/`validateVariableUsage`,
using a `std::map<String,int> _variableSlots` that is built and thrown away after parsing — the *parser*
can keep using `String`-keyed maps freely since it runs once; only the **runtime's hot path** needs to
avoid them.

---

## 4. `DisplayListItem` size, volume, and a compact-POD proposal

### 4.1 Current size (estimated breakdown, `micropatterns_command.h` lines 115–136)

```cpp
struct DisplayListItem {
    CommandType type;                       // 4 bytes (enum, typically int)
    int sourceLine;                         // 4 bytes
    std::map<String, int> intParams;        // sizeof(std::map) ~48 bytes on the struct itself,
                                             //   PLUS one heap allocation (red-black tree node,
                                             //   ~32-48 bytes incl. a String key) PER ENTRY
    std::map<String, String> stringParams;  // same: ~48 bytes inline + heap node per entry
    float matrix[6];                        // 24 bytes
    float inverseMatrix[6];                 // 24 bytes
    float scaleFactor;                      // 4 bytes
    uint8_t color;                          // 1 byte
    const MicroPatternsAsset* fillAsset;    // 4 bytes (32-bit ESP32 pointer)
    bool isOpaque;                          // 1 byte
};
```
Inline struct size is roughly 4+4+48+48+24+24+4+1+4+1 ≈ **~165 bytes before padding**, but the *real* cost
is the heap allocations hiding behind the two `std::map`s: a `DRAW` item has 2 `intParams` entries (X,Y)
+ 1 `stringParams` entry (NAME) → **3 separate heap-allocated tree nodes per item**, each carrying a
`String` key (itself a small heap allocation for anything beyond very short SSO... except Arduino
`String` has no SSO, so even `"X"` as a map key is a heap allocation). A `RECT`/`FILL_RECT` item has 4
`intParams` entries → 4 heap nodes. So **every single display-list item costs somewhere between 2 and 5
extra heap allocations**, on top of the ~165-byte inline struct, on top of whatever heap churn happened
producing the resolved values in the first place (§2).

For the `city.json` script (~20,000 loop iterations, ~1 DRAW/FILL_RECT surviving per iteration due to the
mutually-exclusive IF chain) the display list itself would be on the order of **20,000 items**, i.e.
roughly 20,000 × 165 bytes ≈ **3.3 MB of inline struct storage alone**, before counting the per-item map
node allocations (another 40,000–100,000 heap allocations) — on a device with 4MB PSRAM total. This is a
strong candidate for being not just slow but also a real memory-pressure risk (fragmentation, possible
allocation failures on more complex scripts), independent of raw CPU time.

**This number is a rough estimate from reading the struct layout and the example script's loop count, not
a measured `sizeof()`/heap-profiled figure — worth confirming with an actual on-device
`ESP.getFreeHeap()`/`heap_caps_get_free_size()` trace across the render.**

### 4.2 Compact POD proposal

```cpp
struct DisplayListItem {
    uint8_t  type;         // CommandType fits in a byte
    uint8_t  color;         // 0 or 15 today; keep byte-sized
    uint8_t  isOpaque : 1;
    uint8_t  matrixIndex;   // index into a small deduplicated matrix table (see §5), not inline floats
    int16_t  sourceLine;    // scripts are short; 16 bits is plenty
    int16_t  scaleFactor;   // integer scale factors only ever appear to be used as ints (SCALE FACTOR=int)
    const MicroPatternsAsset* fillAsset; // keep as pointer, or replace with an index into the asset vector

    union {
        struct { int16_t x, y; } pixel;                       // PIXEL, FILL_PIXEL
        struct { int16_t x1, y1, x2, y2; } line;               // LINE
        struct { int16_t x, y, w, h; } rect;                   // RECT, FILL_RECT
        struct { int16_t x, y, r; } circle;                    // CIRCLE, FILL_CIRCLE
        struct { int16_t x, y; uint16_t assetIndex; } draw;    // DRAW (asset resolved to index, not String)
    } params;
};
```
This drops the struct to roughly **24–28 bytes, zero heap allocations per item** (assuming `int16_t`
coordinates are sufficient — the M5Paper canvas is 960×540, well within `int16_t` range, and DRAW/RECT
etc. params are screen coordinates or small counts in every example script seen). For the 20,000-item
`city.json` case that's ~20,000 × 28 bytes ≈ **560 KB**, a roughly **6× memory reduction**, with the CPU
savings from eliminating ~40,000–100,000 map-node allocations being likely larger in wall-clock terms
than the memory savings themselves (small heap alloc/free pairs are not free on this platform).

This requires:
- Asset name (`stringParams["NAME"]` for DRAW) → resolve to an index into `_assets` (already a
  `std::map<String, MicroPatternsAsset>`; either add a parallel `std::vector<const MicroPatternsAsset*>`
  built once at parse time so DRAW commands can carry a small integer index, or keep the raw pointer as
  above and drop the redundant `stringParams["NAME"]` entirely — the renderer only needs the resolved
  asset for drawing, not its name).
- The renderer (`display_list_renderer.cpp`) to be updated to read the union fields instead of
  `intParams.at("X")` etc. — **this is a shared boundary with the rasterizer subagent's scope**; changing
  `DisplayListItem`'s shape requires coordinating with whoever owns `display_list_renderer.cpp`.

---

## 5. Matrix handling: already reasonably good, further win available

`inverseMatrix` is **not** recomputed per display-list item — `matrix_invert()` is only called inside the
`CMD_TRANSLATE`/`CMD_ROTATE` cases of `processCommandForDisplayList` (i.e. once per state-changing
command), and each `DisplayListItem` gets a `memcpy` snapshot of whatever `_currentState.matrix` /
`_currentState.inverseMatrix` are at that moment (`micropatterns_runtime.cpp` lines 275–276). So the
"recomputed per item" concern raised in the task brief does **not** apply as stated — good news, one less
thing to fix.

What **is** true: every display-list item currently stores its own independent 48 bytes of matrix data
(`float matrix[6] + float inverseMatrix[6]`) via `memcpy`, even though, per the `city.json` example, many
consecutive items inside a REPEAT body share the *exact same* transform state (the script does
`COLOR`/`SCALE`/one conditional `DRAW`/`RESET_TRANSFORMS` per iteration — so the matrix is actually
identity for every DRAW in this particular script, but in general many scripts do `TRANSLATE`/`ROTATE`
once and then emit several primitives before changing the transform again).

**Proposal**: maintain a small deduplicated matrix table (`std::vector<MatrixPair>` where `MatrixPair` is
`{float matrix[6]; float inverseMatrix[6];}`) during `generateDisplayList()`. Before pushing a new
`DisplayListItem`, compare the current state's matrix against the table's last entry (cheap: state only
changes on TRANSLATE/ROTATE/RESET_TRANSFORMS/SCALE, which are far rarer than drawing primitives in loop
bodies) — if unchanged, reuse the existing table index; only append a new table entry when the transform
actually changed since the last item. Each `DisplayListItem` then stores a single `uint8_t`/`uint16_t`
`matrixIndex` instead of 48 bytes of floats. For scripts where transform state is set once outside a loop
and many primitives are drawn inside it (a common and idiomatic MicroPatterns pattern), this collapses
what would be thousands of duplicate 48-byte matrix copies into a single table entry. Even in the
worst case (transform changes every single item) it's no worse than today, modulo the one extra
`uint16_t` per item.

---

## 6. JSON/script loading and caching the display list across wakes

### 6.1 Is JSON parsing/S3 fetch a meaningful share of the 10–19s?

`script_manager.cpp` uses `ArduinoJson`'s `JsonDocument` (not `DynamicJsonDocument`, per the "default
allocator" comments — i.e. it already uses the newer ArduinoJson v7 API) for script-list and
execution-state persistence, and `network_manager.cpp` handles the actual S3 fetch of script content.
**Neither of these paths is currently timed** (no `millis()` brackets found in either file). Given
`RenderController::renderScript` takes `script_content` as an already-resolved `String` parameter (fetch
and load happen before `renderScript` is called, likely in `main.cpp`/`system_manager.cpp`), the reported
10–19s figure may or may not include network/JSON time at all, depending on where that figure was
measured from — **this needs to be pinned down**: is "10-19 seconds" wall-clock from wake to display
update (includes WiFi connect + S3 fetch + JSON parse), or just the `renderScript` call (parse DSL +
generate + rasterize)? These are very different problems with very different fixes. Recommend adding
`millis()` brackets around the network fetch and around `loadScriptList`/state JSON operations to settle
this before further optimization work, since fixing the interpreter buys nothing if the dominant cost is
actually WiFi/S3 round-trip time.

### 6.2 Caching the display list across wakes

Looking at `city.json`, the script re-derives its output from `$HOUR`, `$MINUTE`, `$SECOND`, `$COUNTER`
combined via `$seed`, `$time_factor`, `$counter_mod` into per-tile pseudo-randomness — i.e. this
particular script is designed so that **most or all of the display list is time-dependent** by
construction (every tile's pattern choice depends on `$seed`/`$time_factor`, which depend on
`$HOUR`/`$MINUTE`/`$SECOND`/`$COUNTER`). For *this* script, caching the generated display list across
wakes and only patching `$COUNTER`/time-dependent values would not help — nearly every value depends on
time.

However, this is script-specific, not a property of the DSL/runtime in general. Many simpler
MicroPatterns scripts likely draw a fixed layout and only vary color/a small parameter by `$COUNTER`.
**A generically-applicable version of this optimization is hard to do safely without static analysis**
(the compiler-side "invariant hoisting" concept from `compiler.js` §3.1 is exactly this kind of analysis,
generalized to loop invariants rather than whole-script caching). A cheap, safe, non-analysis-based
partial win: if the *previous* render's `$COUNTER`/time values are known and the new ones are known before
generation starts, a script that contains **zero references** to `$COUNTER`/`$HOUR`/`$MINUTE`/`$SECOND`
anywhere in its `VAR`/`LET`/condition/param tokens (this is staticly checkable by a single scan over the
parsed command tree, comparing against `_declaredVariables`/env references) never needs to regenerate its
display list at all after the first render — the parser could set an `isTimeInvariant` flag and
`RenderController` could skip `generateDisplayList()` entirely on subsequent wakes, reusing the cached
`std::vector<DisplayListItem>`. This is a strict, conservative, easy-to-implement check (much simpler
than per-expression invariant hoisting) that would fully eliminate front-half cost for static/decorative
scripts, while correctly falling back to full regeneration for anything referencing time/counter.

---

## 7. Parity impact — what must be mirrored in the JS emulator

- Any change to which values are `int16_t`-clamped (§4.2) must match the JS side's numeric handling, or
  visuals will diverge for scripts using coordinates/sizes outside `int16_t` range (unlikely given the
  960×540 canvas, but pattern `DATA` widths/heights, `REPEAT COUNT` values, and `$VAR` arithmetic
  intermediate results are NOT bounded by canvas size — e.g. `$random_sum` in `city.json` before the `%
  100` could exceed `int16_t` if intermediate arithmetic isn't clamped correctly; only *final*
  DisplayListItem params being int16 is fine, but be careful this doesn't get confused with the
  `_variables`/expression-evaluation integer type, which must stay `int`/32-bit to match JS's
  `evaluateExpression` semantics).
- The "skip regeneration for time-invariant scripts" idea (§6.2) is a **behavioral** difference from the
  JS reference (which always regenerates) only in *when* generation runs, not in *what* it produces — as
  long as the invariance check is conservative (any doubt → treat as time-variant), visual output is
  identical, so this one is safe from a parity standpoint by construction.
- The bytecode/slot-indexing/compact-POD changes (§3, §4) are pure internal-representation changes on the
  C++ side; they don't need to be mirrored in JS at all *unless* the intent is eventually to give
  `compiler.js`'s design authority over both implementations (i.e., port `compiler.js`'s optimization
  passes to C++ rather than reinventing independently) — worth a product decision on whether to converge
  the two codebases' compile strategies or let them diverge as "JS does source-to-function compilation,
  C++ does slot-indexed bytecode," since perfect JS/C++ code-path parity was seemingly never the design
  goal (JS already has two different runtime paths itself, per §3.1).
- Matrix dedup (§5) is purely an internal storage optimization; the *values* produced are bit-for-bit
  identical to today (same `matrix_multiply`/`matrix_invert` calls, just stored once instead of N times),
  so no JS-side changes needed.

---

## 8. Already tried / historical (from `git log`)

Commit history touching these files (`micropatterns_parser.cpp`, `micropatterns_runtime.cpp`,
`micropatterns_command.h`, `script_manager.cpp`), oldest to newest:

- `11fd9e0` "First M5Paper version", `119095d` "first version that actually display the script" — initial
  bring-up.
- `d364a94` "Fix several parsing issues, make the parser and runtime closer to web implementation" —
  explicit parity work, confirms the intent was always to track the JS reference closely (relevant to
  §3.1/§7 above).
- `baec992` "M5Paper: counter works across refreshes; fix pattern scaling; implement drawFilledPixel"
- `28b189a` "yield in runtime and drawing" — added `yield()` calls inside the interpretation loop (visible
  today in `generateDisplayList`'s `commandCounter % 50`/`% 150` and REPEAT's `i % 20`/`% 60` checks).
  This is a **correctness/watchdog fix, not a performance fix** — it exists to stop the watchdog timer
  from firing during long-running generation, which is itself evidence that generation was already known
  to take long enough to risk WDT resets. It does *cost* a small amount of time (periodic `yield()` +
  `esp_task_wdt_reset()` calls) but removing it would be unsafe without addressing the underlying slowness
  first.
- `3c3a6a6` "Fix time reste before each execution" (sic, "reset")
- `173362d` "Fix runtime issues"
- `9e6d3b1` "Improve watchdog timeout", `9359a21` "Stabilization of watchdog issues; 2 minutes sanity check
  for fetch" — more watchdog-related stabilization, again indirect evidence that render time was already
  bumping against timeout budgets.
- `45d6a66` **"compute optimizations"** — this is the one commit whose message directly claims a
  performance-motivated change to this area. Worth a targeted look (not exhaustively diffed in this pass)
  to see exactly what it changed and whether it was effective, since it's the closest thing to prior
  performance work on this exact code.
- `c407e13` "Change REPEAT COUNT syntax, remove the use of TIMES" — DSL surface change, not perf.
- `6c1b5cd` "m5paper task and rtos rearchitecture" — likely moved rendering to a FreeRTOS task, relevant
  context for where `RenderController::renderScript` runs today, but not a front-half interpreter change.
- `76287f2` "improve script loading and saving", `8759166` "load scripts; fallback to default" —
  `script_manager.cpp` evolution, relevant to §6.1.
- `27eda12` "Render use display list with pixel occupancy map" — introduced the occlusion-culling
  (`isOpaque`) concept still present in `DisplayListItem.isOpaque` / `determineItemOpacity()` today; this
  is a rasterizer-side optimization (skip fully-occluded draws) that depends on front-half correctly
  setting `isOpaque`, so it's a shared concern with the other subagent's scope.
- `29071e7` "fail text don't erase drawing; move jsondoc to heap; don't pass scripts as strings" —
  **"move jsondoc to heap"** and **"don't pass scripts as strings"** both sound like they could be memory-
  or copy-avoidance changes relevant to this analysis; worth a closer diff read if pursuing §6/JSON
  optimizations, since it suggests someone already identified String-copying of full script bodies as a
  problem in the loading path (a sibling problem to the `String` churn described in §2, just at the
  script-loading layer instead of the interpretation layer).
- `04b00d5` "graphical UI feedback; better file lock handling" — most recent, UI-focused.

**No commit in this history touches `micropatterns_command.h`'s core data structures** (no PR ever moved
away from `std::list`/`std::map<String,...>` for commands/params/variables) — i.e. **the data-structure
choices flagged in §2 have never been revisited since the initial port from the JS-parity-focused
`d364a94`**. This strongly suggests the interpreter's algorithmic/data-structure shape has been out of
scope for prior perf work, which instead focused on watchdog safety, task architecture, and script-loading
plumbing. That makes §2/§3's proposals genuinely novel territory for this codebase rather than something
already tried and abandoned — worth flagging as the highest-confidence "under-explored" area.

---

## 9. Ranked proposals

| # | Mechanism | Est. speedup | Effort | Parity impact |
|---|---|---|---|---|
| 1 | Add `millis()` instrumentation around `_parser.parse()`, script fetch, and JSON load/save; collect real device traces across several real scripts | N/A (diagnostic) | S | None |
| 2 | Slot-indexed variables/env (§3.3): replace `String`-keyed `_variables`/`_environment` maps with `std::vector<int>`/fixed array indexed by parse-time-assigned slot | **Est. large** — removes the single biggest source of String-alloc + map-lookup churn identified in §2.3/§2.4 (potentially the majority of the ~1M String allocs estimated for `city.json`) | M | None (internal only) |
| 3 | Struct-of-fields (or slot-indexed) command params instead of `std::map<String,ParamValue> params` (§2.2, §3.2 item 3) | **Est. moderate-large**, compounds with #2 | M | None |
| 4 | Flatten `std::list<MicroPatternsCommand>` → `std::vector` with jump offsets (§3.2 item 1) | **Est. moderate** — locality win, especially with PSRAM; also removes recursive-list-walk-per-outer-iteration for nested REPEAT | M–L (touches parser's block-stack logic and runtime's traversal) | None |
| 5 | Compact POD `DisplayListItem` (union of per-type params, int16 coords, no `std::map`) (§4.2) | **Est. large** for scripts with big display lists — removes 2-5 heap allocs per item and ~6× memory | M (requires renderer coordination — shared boundary) | None to visuals; requires renderer-side changes in lockstep |
| 6 | Matrix table dedup: `matrixIndex` instead of inline 48-byte matrix pair per item (§5) | **Est. small-moderate**, mostly memory not CPU, bigger win for transform-then-many-primitives scripts than for city.json-style per-iteration RESET_TRANSFORMS scripts | S–M | None |
| 7 | Static "time-invariant script" detection → skip regeneration entirely on subsequent wakes (§6.2) | **Est. very large** for scripts that qualify (100% of front-half cost eliminated after first render), **zero** benefit for scripts like `city.json` that reference time/counter throughout | S–M (single scan at parse time + cache lifecycle in RenderController) | None (conservative, falls back safely) |
| 8 | ~~Port `compiler.js`'s design to C++~~ **[CORRECTED — see §3.1b]** The compiler path is the path that LOST the benchmark to the display list; do not port it as a pipeline. Its individual techniques (invariant hoisting, constant folding) may still be worth applying *inside* the display-list generator, but must be justified and measured independently | **Est. large**, but this subsumes #2-#4 and adds finer-grained wins (loop-invariant hoisting inside a still-executed loop) | L | No parity gap exists to close — the C++ display-list mirror is already the benchmark-winning design. Any technique borrowed from the compiler must be mirrored into `display_list_generator.js` to preserve parity |

Ranking guidance: **#1 should happen first regardless of what else is chosen** — it's cheap and de-risks
every other estimate in this document. **#2 and #3 together are the best effort-to-confidence ratio** —
they directly attack the mechanism (String+map churn) that this analysis has the most (though still
circumstantial) evidence for being dominant, are purely internal (zero parity risk), and don't require
coordinating with the rasterizer subagent. **#5 has the largest headline number but requires cross-team
coordination** since `DisplayListItem`'s shape is a shared contract with the renderer. **#7 is the
cheapest way to get a large win for a subset of real-world scripts** and is worth doing in parallel with
the others since it's orthogonal.

---

## 10. Unknowns and what we'd need to measure

- **Parse-time vs. generation-time split** — not measured at all (§1). Could change prioritization
  significantly if parsing (e.g., large `DEFINE PATTERN DATA=...` strings) turns out to be non-trivial.
- **Where does the 10–19s figure come from?** Is it wall-clock wake-to-display (includes WiFi/S3), or just
  `renderScript()`? (§6.1) This single unknown could redirect the entire optimization effort toward
  networking instead of the interpreter.
- **No internal profiling of `generateDisplayList()`** — the String-allocation and map-traversal counts in
  §2.4 are derived by reading the code and counting operations per the `city.json` example by hand, not
  from a profiler or instrumented build. A quick win: add a build-time-flagged counter
  (`#ifdef MP_PROFILE`) incrementing on every `String` temp / map lookup in the hot functions, or simpler,
  just bracket `resolveValue`/`evaluateExpression`/`processCommandForDisplayList` with cumulative
  `micros()` timers for one test run.
- **Actual heap fragmentation/allocation-failure behavior** on-device for large scripts — is
  `ESP.getFreeHeap()` (or PSRAM-specific equivalent) ever logged before/after a render? Not found in this
  pass; would confirm or refute the §4.1 memory-pressure concern.
- **Distribution of real-world scripts.** Only one real example script was available for this analysis
  (`city.json`, a nested-REPEAT ~20K-iteration script) plus the top-level `cases/` directory, which
  appears to be empty (only a `.DS_Store` was found — worth checking if example scripts live elsewhere,
  e.g. `micropatterns_server/dev/` or the editor's built-in examples, since a single data point
  substantially under- or over-states how loop-heavy typical scripts are). This materially affects how
  much §2/§4's estimates generalize.
- ~~**Whether `compiler.js`/`compiled_runtime.js` is live, dead, or partially-wired code on the JS side**~~ **[RESOLVED — see §3.1b]** It is a superseded but still-selectable comparison path; `displayList` is the default. Remaining sub-question, still open:
  (§3.1) — the constructor comments in `compiled_runtime.js` read as somewhat unfinished ("not yet set
  here", "will be handled by passing it to drawing methods... or we enhance later"). If this pipeline
  isn't actually exercised by the current editor UI, it's still useful as a *design reference* for C++,
  but shouldn't be assumed to be a battle-tested, currently-relied-upon implementation.
- **Commit `45d6a66` "compute optimizations"** was not diffed in detail during this pass — recommend
  reading its full diff before starting new optimization work, to avoid duplicating or contradicting
  whatever it already changed.
