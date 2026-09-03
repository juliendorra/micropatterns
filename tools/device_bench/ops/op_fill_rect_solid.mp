# Solid FILL_RECT covering the canvas once. The cheapest per-pixel path there
# is: no pattern lookup, no per-pixel divide, full-width spans.
#
# Coverage is ONE canvas per render, not N. Repeating a full-canvas fill is
# pointless -- the occlusion buffer correctly culls every item after the first,
# so 24 copies measured exactly the same work as 1. Statistics come from reps.
COLOR NAME=BLACK
FILL NAME=SOLID
RESET_TRANSFORMS
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
