# 2026-09-03 — The command tree is gone: scripts compile to a flat program, once, at sync

## What changed

The parser no longer builds a tree of `MicroPatternsCommand` nodes (a `std::list` of
~256-byte structs, each owning a `std::map<String, ParamValue>` and three token vectors of
`String`s). It compiles each line straight into an `MpProgram` (`mp_program.h`):

- `code` — one 48-byte `MpInstr` per command; REPEAT / IF / ELSE carry jump targets.
- `exprs` — an 8-byte operand pool; VAR / LET / IF expressions are slices of it, in postfix,
  precedence already resolved.
- `assets` — DEFINE PATTERN bitmaps by index; FILL / DRAW refer to them by index.
- Variables are slots (seven env slots, then user variables); parameters are positional.

The runtime (`micropatterns_runtime.cpp`) is now a small VM: a program counter, a loop stack,
a value stack for expressions, and one `int32_t` array of variables. Nothing is looked up by
name and nothing is allocated per instruction.

The program is serializable (24-byte header with format version, source length + CRC, body
length + CRC). Both firmwares compile every script **at the end of a sync, after WiFi is
down**, and store the bytes at `/scripts/compiled/<fileId>.mpc`. A render then streams the
stored program straight into its containers (`ScriptManager::loadProgram`), never holding
the file in RAM as a whole and never parsing. A stored program that does not match the source
on flash (streamed CRC) or the current format version is thrown away and recompiled. Boot
warms the cache (`compileAllPrograms`) so no render has to compile lazily — including the
first boot after a firmware update, when every stored program carries the old format.

## Why

Measured across the twelve device scripts (`docs/explainers/`), the parse tree outweighed the
display list it produced by 10–50×: ~146 KB for Seascape II (387 commands) against a peak
display list of 11.6 KB. It was also thousands of small `String` and tree-node allocations,
which is what fragmented the Watchy's heap. The button-press reboot on Seascape II was that
parse happening after `MPProvisioning::openWindow()` had taken ~90 KB for the BLE stack.

## Equivalence

Output is byte-identical to the tree walker, checked three ways before the old code was
deleted:

- `mpharness verify`: 18/18 goldens.
- `mpharness compare-paths displaylist compiled`: 18 identical (the `compiled` path
  serializes and deserializes before running).
- An md5 oracle of every corpus script × 2 canvases × 24 clock seeds, rendered by the
  pre-change binary: **576/576 identical** after the change.

Behaviours that had to be reproduced on purpose rather than "fixed": a missing parameter is
the literal default the old runtime substituted; a parameter of the wrong type warns at compile
time and uses the default; `-` inside an IF condition is always an operator (so `$x < -5`
compares against 0, as it always did); a condition without exactly one comparison is false at
runtime with an error; a variable used in a parameter but never declared gets a slot and reads
as "Undefined variable" → 0; DEFINE PATTERN may follow its first use.

## Measurements

Host harness, idle machine, medians of 9 runs. Parse is unchanged (0.9–1.0×). Display-list
generation:

| script | canvas | before ms | after ms | × |
|---|---|---:|---:|---:|
| Seascape II | 200×200 | 0.127 | 0.048 | 2.6 |
| Seascape II | 960×540 | 0.364 | 0.182 | 2.0 |
| City 2 | 200×200 | 0.018 | 0.002 | 9 |
| Circuits | 200×200 | 0.014 | 0.002 | 7 |
| Eyes | 200×200 | 0.041 | 0.015 | 2.7 |
| harness city.mp (20,737 items) | 960×540 | 12.34 | 8.77 | 1.4 |

Loading a stored program instead of parsing: 0.1–0.4 ms on the host for the biggest scripts.

Memory, constrained WASM Watchy profile (versioned ESP-IDF allocator, `verify_constrained.mjs`):

| | before | after |
|---|---|---|
| Seascape II, radios off | OOM in display-list generation (parse tree had taken the heap) | compile peak 74 KB; render from stored program peak 68 KB |
| Seascape II, BLE active | OOM | render from stored program peak 68 KB, pixels identical |
| City 2 | — | compile 46 KB; render 29 KB |

On the Watchy itself (serial, this firmware): boot warm-up compiled 11 programs in ~8 s,
Seascape II being 448 instructions / 32,715 B in RAM / 31,013 B on flash, and the largest free
block was **110,580 B before and after** — no fragmentation. The 77-second re-render of Eyes:
program loaded in 25 ms, generation 2 ms. A full sync of 12 scripts ends with a Compile stage
after WiFi is down.

## Storage safety, done alongside

- `SPIFFS.begin(false)`: a mount failure is reported, never repaired by formatting. The
  scripts on that partition may be the only copy. `ScriptManager::formatStorage()` is called by
  the sync path only, because a sync is what refills the partition.
- Every writer (content, `list.json`, `current_script.id`, `script_states.json`, `.mpc`) goes
  through `writeFileAtomic_nolock`: write `.tmp`, verify size, remove old, rename. The window
  shrinks from "the whole write" to two metadata operations. LittleFS would close it.
- The stored program is a cache: header + two CRCs, and anything that does not match is
  recompiled. It never needs to survive; it needs to be detected as stale.
- On the Watchy, a render that would have to compile lazily closes the BLE window first.

## Emulator fidelity

The WASM builds compile the same core sources and now expose the device's two stages:
`mp_compile(src)` (radios off, as a sync) keeps the bytes as the "file on flash";
`mp_render(...)` loads from them in the selected radio state, or compiles lazily if no
stored program matches — the device's lazy path. `device_renderer.js` always compiles first,
so a compile failure is reported as a sync-time event and a render failure as a wake-time one.
The serialized bytes are host memory in the simulator, not device heap, because on the device
they are on flash and are streamed in.

## Format

`MP_PROGRAM_FORMAT_VERSION` is 1. Bump it whenever `MpInstr`, `MpOperand`, `CommandType`
values or the serialized layout change; stored programs with another version are recompiled
from source on the next boot.
