# Where the real scripts actually spend their time — 2026-09-04

Per-drawing-operation profile of the twelve scripts on the device, measured on a
Watchy on the default render path. Built because three separate optimisations
this week were aimed by intuition and all three got the cause wrong.

`env:watchy2-profile` times every display-list item and buckets by command type;
`collect_watchy.py --profile` reports it.

## Result

| script | total | dominant operation | share |
|---|---|---|---|
| `art_deco_4` | 133.15 ms | **FILL_CIRCLE** (54 items) | **68.5%** |
| `seascape_4` | 96.39 | DRAW (117) | 71.2% |
| `thunderstorms` | 73.94 | FILL_RECT (21) / DRAW (11) | 48.5% / 45.9% |
| `seascape_2` | 58.19 | FILL_RECT (122) | 57.7% |
| `disconnected` | 55.42 | FILL_RECT (12) | 92.4% |
| `eyes` | 45.37 | FILL_RECT (1) | 57.9% |
| `circuits` | 43.96 | FILL_RECT (1) / DRAW (4) | 47% / 45% |
| `confetti` | 43.44 | FILL_RECT (1) | 66.0% |
| `city_2_by_telohtrab` | 43.39 | FILL_RECT (1) | 52.8% |
| `reconnected` | 42.08 | FILL_RECT (18) | 90.6% |
| `city_by_telohtrab` | 38.76 | FILL_RECT (1) | 65.7% |
| `grid` | 38.55 | FILL_RECT (1) | 91.8% |

**`FILL_RECT` and `DRAW` are the renderer.** Between them they are the dominant
cost of eleven of the twelve scripts. `LINE` appears once, at 3.6%. `PIXEL`,
`RECT` and `CIRCLE` never appear at all: no real script uses them in a way that
costs anything.

Worth stating plainly, because the per-operation probe corpus gives all ten
operations equal billing and the real art does not.

**`art_deco_4` is the exception and the outlier.** At 133 ms it is the heaviest
script on the device, and **68.5% of it is filled circles** — 91 ms across 54 of
them. It is the only script where `FILL_CIRCLE` really matters, `seascape_2`
(27.2%) aside.

## What this settles

The integer-transform measurement found `art_deco_4` regressing **+13.2%** and
`seascape_2` **+5.3%**, and attributed it to those being the only two scripts
containing `FILL_CIRCLE` — inferred by grepping the source text, which is weak
evidence. This profile confirms it from what actually renders, and supplies the
magnitude the grep could not.

It also shows the attribution was incomplete. 68.5% of `art_deco_4` being filled
circles, against a +2.6% per-operation cost, predicts about **+1.8%** — not
+13.2%. The gap is the shape of the circles: `op_fill_circle` is ONE disk of
radius 100 covering 31,000 pixels, while `art_deco_4` draws **54 small** ones at
1,689 us each. A big circle amortises per-row and per-item overhead; a small one
does not.

Same lesson `op_fill_pixel` taught the same day, in a different costume: a probe
sized so one operation's inner loop dominates will hide exactly the per-item
costs that real art pays over and over.

## And it does NOT justify the Bresenham circle span

Which is what this profile was run to decide. The answer is no.

`FILL_CIRCLE` being 68.5% of `art_deco_4` says circles are where the time is. It
does not say the SPAN is. Working it out: 91 ms across roughly 150,000 filled
pixels is about 0.6 us per pixel, some 145 cycles at 240 MHz. That is the
per-pixel fill — the Q16.16 walk, the pattern lookup, `emitPixel` — not the
handful of scanline setups around it. The span is one `sqrtf` and a few
operations per ROW, against tens of pixels in that row.

Earlier evidence already pointed the same way and was sufficient: replacing
`sqrtf` with an integer square root made `op_fill_circle` *slower*, twice, with
two different algorithms. A cost that does not appear when you remove it is not
a cost.

**So the target in `art_deco_4` is the per-pixel path, not the span** —
`emitPixel`, the occupancy map, the pattern lookup. That code has been untouched
all week, and `op_fill_rect_solid` (33 ms, +0.0% through every single change
made) has been quietly pointing at it the whole time.

## The instrument was wrong first, again

The first run reported `art_deco_4` as **73% unaccounted** — 133 ms total against
36 ms of timed items — which reads as a spectacular finding about time being
spent outside drawing entirely.

It was a bug in the profiler. The accumulator array was `kProfileTypes = 16`,
and in `CommandType` `CMD_CIRCLE` is 16 and `CMD_FILL_CIRCLE` is 17, so the
bounds check `t < kProfileTypes` silently dropped every circle — in the one
script that is mostly circles.

Caught by cross-checking two counters that should have agreed and did not: the
renderer reported `rendered=67` while the profiler had timed 13 items. Neither
number is interesting alone; the discrepancy is.

Fourth instrument artifact this week, after `--after no_reset` read as device
state, macOS lacking `timeout`, and "serial is worthless on this device". The
pattern is consistent enough to state as a rule: **when a measurement says
something remarkable, check the instrument before believing the result.**
