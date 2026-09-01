# =====================================================================
# Pseudo-random numbers in MicroPatterns -- reference example
#
# MicroPatterns has no RANDOM command. Scripts that want variety build
# a generator out of LET, and the same three mistakes keep appearing.
# This script is the shape that works, and it draws its own proof: a
# scatter that should look structureless, and a histogram of 200 draws
# that should sit near the expected line.
#
# THE GENERATOR
#   LET $seed = $seed * 2053 + 13849
#   LET $seed = $seed % 32749
#
# RULE 1 -- keep every intermediate inside a 32-bit int.
#   The device evaluates in int32 and wraps on overflow; the browser
#   used doubles until the emulator was taught to wrap too. A textbook
#   constant like 1103515245 times a 15-bit seed is ~3.6e13, which wraps
#   negative on the watch and stays positive in a browser: the same
#   script draws two different pictures. Here the largest intermediate
#   is 32748 * 2053 + 13849 = 67,247,893, about 1.6% of the int32 range.
#
# RULE 2 -- the modulus must not be a power of two.
#   Modulo 32768, bit k of the state repeats every 2^(k+1) draws: bit 0
#   alternates, bit 4 cycles every 32. Anything that takes a small
#   modulo of the state reads exactly those bits, so `% 4` lands on a
#   32-draw loop and `% 10` can make some outcomes unreachable. 32749 is
#   the largest prime below 2^15 and has no such structure.
#
# RULE 3 -- do not compute a "random" value from $INDEX or from x and y.
#   $x * 13 + $y * 19 + seed is affine in x and y, so its value marches
#   in a constant step along every row and every diagonal. It looks like
#   arithmetic because it is. Advance the generator once per item
#   instead, in visit order, and let the state carry the randomness.
# =====================================================================

# ---- seed ----------------------------------------------------------
# Mix several environment values so the first draw differs between two
# renders in the same minute. $COUNTER carries the most entropy: on the
# Watchy it is per script and survives a reboot.
VAR $seed = $SECOND * 137 + $COUNTER * 2749 + $MINUTE * 31 + $HOUR * 907
LET $seed = $seed % 32749

VAR $rnd = 0

# Discard a few draws. A small seed makes a small first output, which
# would put the first sample in the top-left corner every time.
REPEAT COUNT=4
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
ENDREPEAT

# ---- layout --------------------------------------------------------
VAR $plot_h = $HEIGHT / 4
VAR $plot_top = $HEIGHT - $plot_h
VAR $field_h = $plot_top - 4
VAR $bar_w = $WIDTH / 10
VAR $bar_ink = $bar_w - 2
VAR $samples = 200
VAR $full_scale = 40

COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
COLOR NAME=BLACK

# =====================================================================
# Part 1 -- a scatter field
#
# Two draws per dot, taken in visit order. Structureless is the whole
# point: rows, diagonals or clumps here mean the generator is wrong.
# =====================================================================

VAR $dot_size = $WIDTH / 100
IF $dot_size < 2 THEN
  LET $dot_size = 2
ENDIF
VAR $dot_x = 0
VAR $dot_y = 0
VAR $span_x = $WIDTH - $dot_size
VAR $span_y = $field_h - $dot_size

REPEAT COUNT=90
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $dot_x = $rnd % $span_x

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $dot_y = $rnd % $span_y

  FILL_RECT X=$dot_x Y=$dot_y WIDTH=$dot_size HEIGHT=$dot_size
ENDREPEAT

# =====================================================================
# Part 2 -- 200 draws, bucketed by `% 10`
#
# `% 10` is the case that exposed the power-of-two modulus: 10 shares a
# factor with 2, so it reads the short-period low bits. With a prime
# modulus the ten bars land near the expected line. With 32768 some
# bars would be empty and others would take a third of the draws.
#
# There are no arrays, so the ten buckets are ten variables and the
# dispatch is an IF chain.
# =====================================================================

VAR $b0 = 0
VAR $b1 = 0
VAR $b2 = 0
VAR $b3 = 0
VAR $b4 = 0
VAR $b5 = 0
VAR $b6 = 0
VAR $b7 = 0
VAR $b8 = 0
VAR $b9 = 0
VAR $bucket = 0

REPEAT COUNT=$samples
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $bucket = $rnd % 10

  IF $bucket == 0 THEN
    LET $b0 = $b0 + 1
  ENDIF
  IF $bucket == 1 THEN
    LET $b1 = $b1 + 1
  ENDIF
  IF $bucket == 2 THEN
    LET $b2 = $b2 + 1
  ENDIF
  IF $bucket == 3 THEN
    LET $b3 = $b3 + 1
  ENDIF
  IF $bucket == 4 THEN
    LET $b4 = $b4 + 1
  ENDIF
  IF $bucket == 5 THEN
    LET $b5 = $b5 + 1
  ENDIF
  IF $bucket == 6 THEN
    LET $b6 = $b6 + 1
  ENDIF
  IF $bucket == 7 THEN
    LET $b7 = $b7 + 1
  ENDIF
  IF $bucket == 8 THEN
    LET $b8 = $b8 + 1
  ENDIF
  IF $bucket == 9 THEN
    LET $b9 = $b9 + 1
  ENDIF
ENDREPEAT

# ---- plot ----------------------------------------------------------
# Baseline, then one bar per bucket.

FILL_RECT X=0 Y=$plot_top WIDTH=$WIDTH HEIGHT=1


VAR $count = 0
VAR $bar_h = 0
VAR $bar_x = 0
VAR $bar_y = 0
VAR $bar_top = $plot_top + 4
VAR $bar_max = $plot_h - 4

REPEAT COUNT=10
  LET $count = 0
  IF $INDEX == 0 THEN
    LET $count = $b0
  ENDIF
  IF $INDEX == 1 THEN
    LET $count = $b1
  ENDIF
  IF $INDEX == 2 THEN
    LET $count = $b2
  ENDIF
  IF $INDEX == 3 THEN
    LET $count = $b3
  ENDIF
  IF $INDEX == 4 THEN
    LET $count = $b4
  ENDIF
  IF $INDEX == 5 THEN
    LET $count = $b5
  ENDIF
  IF $INDEX == 6 THEN
    LET $count = $b6
  ENDIF
  IF $INDEX == 7 THEN
    LET $count = $b7
  ENDIF
  IF $INDEX == 8 THEN
    LET $count = $b8
  ENDIF
  IF $INDEX == 9 THEN
    LET $count = $b9
  ENDIF

  LET $bar_h = $count * $bar_max / $full_scale
  IF $bar_h > $bar_max THEN
    LET $bar_h = $bar_max
  ENDIF
  IF $bar_h < 1 THEN
    LET $bar_h = 1
  ENDIF

  LET $bar_x = $INDEX * $bar_w + 1
  LET $bar_y = $bar_top + $bar_max - $bar_h
  FILL_RECT X=$bar_x Y=$bar_y WIDTH=$bar_ink HEIGHT=$bar_h
ENDREPEAT

# The expected level, drawn in white over the bars: 200 draws over 10
# buckets is 20 each, and $full_scale = 40 reaches the top of the plot,
# so it sits at half height. Bars should straddle it, not clear it.
COLOR NAME=WHITE
VAR $expected_y = $bar_top + $bar_max / 2
FILL_RECT X=0 Y=$expected_y WIDTH=$WIDTH HEIGHT=1
COLOR NAME=BLACK

# =====================================================================
# CHECKING A GENERATOR
#
#   Even bars, and a scatter with no visible rows or diagonals, at
#   several values of $COUNTER and $SECOND. A generator can be wrong
#   without drawing a single wrong pixel -- the failure is a
#   distribution, so a golden-image comparison will pass either way.
#
#   The quick test for the power-of-two trap: take ($seed / 8) % 4 for
#   64 draws and compare the first 32 to the second 32. Modulo 32768
#   they are identical. Modulo 32749 they are not.
#
# WHEN A DIFFERENT RANGE IS NEEDED
#   $rnd % $n            0 .. n-1
#   $rnd % $n + $lo      lo .. lo+n-1
#   Guard $n: `% 0` is a runtime error, so clamp a computed range to at
#   least 1 before using it.
#
# MAKING AN OUTCOME RARE
#   Draw, and if the result lands in the unwanted band, draw once more
#   and keep the second value. Two draws must agree to reach the band,
#   so its share falls from p to about p squared without making it
#   unreachable.
# =====================================================================
