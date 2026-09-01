# Deliberately structured seed set: the clock only ever sets high bits, so the
# low bits of the seed are constant. With a power-of-two modulus the low bits of
# the first output are an affine function of those, so they are constant too --
# and the level, read from them, cannot take every value.
#
# This exists to prove `sweep` detects unreachability at all. If it ever reports
# "unreachable: none" here, the tool has stopped working.
VAR $seed = 0
VAR $level = 0
VAR $i = 0
LET $seed = $HOUR * 1024
LET $seed = $seed * 2053
LET $seed = $seed + 13849
LET $seed = $seed % 32768
LET $seed = $seed + 32768
LET $seed = $seed % 32768
LET $level = $seed / 8
LET $level = $level % 10
LET $level = $level + 1
COLOR NAME=BLACK
REPEAT COUNT=$level
  LET $i = $INDEX * 3
  PIXEL X=$i Y=0
ENDREPEAT
