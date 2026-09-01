# city.mp's REAL zoom block, instrumented for `mpharness sweep`.
# Not an approximation: the seed construction, constants, step count and
# re-roll below are copied from examples/scripts/city.mp. It draws exactly
# $scaling isolated pixels, so the non-white pixel count IS the level.
#
# sweep_city_32749.mp must PASS  -- all ten levels reachable.
# sweep_city_32768.mp must FAIL -- levels 3, 4, 7 and 9 never occur.
#
# The bug is NOT a stream property. city.mp re-seeds from the clock every
# render and consumes one or two draws, so what matters is the seed-to-
# first-output map over a seed set that is itself linear in the clock.
# Modulo 2^15 that map is affine on the low bits and the structure
# survives; a prime modulus mixes across all bits and destroys it. A long
# run of draws from one seed looks uniform under BOTH moduli and cannot
# show this.
#
#   make sweep S=probe/corpus/sweep_city_32749.mp LEVELS=10
VAR $seed = $HOUR * 60 + $MINUTE + $COUNTER
VAR $time_factor = $SECOND * 3 + $MINUTE / 10
VAR $counter_mod = $COUNTER % 17
VAR $scale_random = $seed + $time_factor * 7 + $counter_mod * 11
VAR $rng = $scale_random % 32749

VAR $max_scaling = $WIDTH / 20
IF $max_scaling > 12 THEN
    LET $max_scaling = 12
ENDIF
VAR $min_scaling = 1
IF $WIDTH >= 400 THEN
    LET $min_scaling = 2
ENDIF
IF $max_scaling < $min_scaling THEN
    LET $max_scaling = $min_scaling
ENDIF
VAR $scale_steps = $max_scaling - $min_scaling + 1
VAR $rare_from = $scale_steps - 2
IF $rare_from < 1 THEN
    LET $rare_from = 1
ENDIF

LET $rng = $rng * 2053 + 13849
LET $rng = $rng % 32749
VAR $scale_pick = $rng / 8 % $scale_steps
IF $scale_pick >= $rare_from THEN
    LET $rng = $rng * 2053 + 13849
    LET $rng = $rng % 32749
    LET $scale_pick = $rng / 8 % $scale_steps
ENDIF
VAR $scaling = $min_scaling + $scale_pick

COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
COLOR NAME=BLACK
VAR $px = 0
REPEAT COUNT=$scaling
  LET $px = $INDEX * 3
  PIXEL X=$px Y=0
ENDREPEAT
