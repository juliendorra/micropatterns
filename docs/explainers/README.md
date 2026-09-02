# Explainers

Two visual notes on the renderer, written 2026-09-02. Open the `.html` files in a
browser; they are self-contained apart from Google Fonts.

- `tree-walk-or-bytecode.html` — how a script becomes pixels on both devices
  today, what a bytecode VM would change, and why the parsed program (not the
  display list) is the structure that keeps breaking on the Watchy. Includes the
  Seascape II button-re-run reboot analysis.
- `which-way-round-to-paint.html` — display list versus streaming renderer:
  what each can skip, what each costs, measured across twelve scripts.

## data/

Produced by `tools/host_harness` (`build/mpharness render`), host timings only —
they compare algorithms, not devices. See the harness README before quoting.

- `items-by-clock-seed.tsv` — display-list item counts and counters for every
  script at 960×540 and 200×200, swept over 24 clock seeds (`sweep.sh`).
- `phase-timing-map-ab.tsv` — parse/generation/rasterization phase timings,
  9 reps, `displaylist` vs `displaylist-nomap` (`time.sh`). The two paths are
  byte-identical in output (`mpharness compare-paths`).
- `run.sh` — single-seed run of everything, both paths, both canvases.

Corpus: the six scripts in `tools/device/backups/2026-08-27-143524/files/scripts/content/`
(s0 Circuits, s1 City 2, s2 City, s3 Eyes, s4 Re/Connected, s5 Thunderstorms),
`examples/scripts/seascape{,2,3}.mp`, and `artdeco_default`, `emulator_welcome`,
`city` from `tools/host_harness/corpus/`. The scripts adjust with `$WIDTH`/`$HEIGHT`
and derive their tile scale from the clock, hence the seed sweep.
