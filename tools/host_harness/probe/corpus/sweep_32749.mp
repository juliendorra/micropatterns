# Seed-to-first-output map, modulus 32749.
#
# Mirrors the shape city.mp uses: RE-SEED from the clock every render, take ONE
# draw, choose a level. What matters is therefore not the LCG's stream (which is
# uniform over a full period, and so can never show this) but the map from the
# seed set the clock actually produces to the first output.
#
# The level is encoded as a COUNT of isolated pixels, so the non-white pixel
# count of the frame IS the chosen level and a sweep can tally it.
VAR $seed = 0
VAR $t = 0
VAR $level = 0
VAR $i = 0
LET $seed = $HOUR * 3600
LET $t = $MINUTE * 60
LET $seed = $seed + $t
LET $seed = $seed + $SECOND
LET $t = $COUNTER * 7
LET $seed = $seed + $t
LET $seed = $seed % 32749
LET $seed = $seed * 2053
LET $seed = $seed + 13849
LET $seed = $seed % 32749
LET $seed = $seed + 32749
LET $seed = $seed % 32749
LET $level = $seed / 8
LET $level = $level % 10
LET $level = $level + 1
COLOR NAME=BLACK
REPEAT COUNT=$level
  LET $i = $INDEX * 3
  PIXEL X=$i Y=0
ENDREPEAT
