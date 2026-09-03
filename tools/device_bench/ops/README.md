# Per-operation benchmark corpus

One script per drawing operation, each sized so the primitive's inner loop
dominates its own runtime. Deliberately different from
`tools/host_harness/probe/corpus/`, which isolates primitives for OCCLUSION
probing with 12 tiny items -- too little work to time.

Design rules, so a number from one script is comparable to another:

- **Few items, large areas.** The display list costs ~370 bytes per item and the
  Watchy has no PSRAM, so coverage comes from big shapes repeated ~20-40 times,
  not from thousands of small ones.
- **Canvas-relative coordinates** via `$WIDTH` / `$HEIGHT`, so the same script
  does proportional work on a 200x200 Watchy and a 960x540 M5Paper.
- **Paired rotated / unrotated variants.** An unrotated inverse transform has
  `im1 == 0`, which lets the rasteriser hoist the pattern row out of the
  scanline; a rotated one cannot. That pair is where a fixed-point transform
  would show up, so it has to be measurable separately.
- **Integer-path probes included on purpose.** `line`, `rect_outline` and
  `circle_outline` rasterise through integer Bresenham/midpoint code that a
  fixed-point transform port does not touch. They are the control group: if they
  move, the change leaked somewhere it should not have.
