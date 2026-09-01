# An LCG read through its LOW bits, reduced modulo 32768.
#
# The two extra lines after the modulo force the seed non-negative: C's % keeps
# the sign of the dividend, and the multiply overflows int32 into negatives, so
# without them the sequence is not an LCG at all -- it is an LCG plus a sign
# flip, which destroys the very periodicity this is meant to show.
VAR $seed = 1
VAR $pick = 0
VAR $x = 0
COLOR NAME=BLACK
FILL NAME=SOLID
REPEAT COUNT=$COUNTER
  LET $seed = $seed * 1103515245 + 12345
  LET $seed = $seed % 32768
  LET $seed = $seed + 32768
  LET $seed = $seed % 32768
ENDREPEAT
LET $pick = $seed / 8
LET $pick = $pick % 4
LET $x = $pick * 40
LET $x = $x + 10
FILL_RECT X=$x Y=10 WIDTH=30 HEIGHT=30
