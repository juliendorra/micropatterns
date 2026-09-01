DEFINE PATTERN NAME="eye1" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000100100100010001000001001001000100110000011011011111001001000010011100011110110000111100000001111000111000011111000010001000001110011000010010000111100011000100100001111000110001001100011110011100010001100011111110001100001100011111100010000000110001110001000000000111000000100000000000011111110000000000000000000000000000000000000000000000000000000000000000000000000000000000"
DEFINE PATTERN NAME="eye2" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000000000000000000000000000000000000000000001111110000000000001100000010000000000110000000010000000001000011100010000000110001111100010000001000111111100100000010001110011001000000100011100110010000001000111111100100000010001111110010000000010001111000100000000110000000011000000000110000001100000000000011111110000000000000000000000000000000000000000000000"
DEFINE PATTERN NAME="eye3" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000010000000000000001000101001001001000010001010010010010000110010110100010100000100110101001001000001110111111111110000010000000000111000001100001111110100000110000111110101000000100001111101010000000100011111110100000001000111111110000000001001111110100000000011000011011000000000011000000100000000000001111110000000000000000000000000000000000000000000000"
DEFINE PATTERN NAME="eye4" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000010001001000000000100100010001000000001000100010010000000011000100100100000010010001001001000000100110111111100000000100110000000100000001001001111100100000001110111111101000000000101100111010000000001011011110100000000010111111101000000000100111110010000000001000011001000000000001000000100000000000001111110000000000000000000000000000000000000000000000"

# ---- background ----
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

# ---- how big may an eye get on THIS screen ----
# The pattern is 20x20, so scale factor f draws a 20*f pixel eye.
# Both bounds are derived from $WIDTH: the biggest eye still fits a
# 200px panel, the smallest still reads on a 540px one.
VAR $max_factor = $WIDTH / 64
IF $max_factor < 1 THEN
  LET $max_factor = 1
ENDIF
VAR $min_factor = $WIDTH / 200
IF $min_factor < 1 THEN
  LET $min_factor = 1
ENDIF
VAR $factor_range = $max_factor - $min_factor + 1
IF $factor_range < 1 THEN
  LET $factor_range = 1
ENDIF

# ---- how many eyes ----
VAR $num_eyes = $WIDTH / 60 * $HEIGHT / 60 + 4
IF $num_eyes > 40 THEN
  LET $num_eyes = 40
ENDIF

# ---- LCG, prime modulus, every intermediate well inside a 32-bit int ----
# The modulus must NOT be a power of two: with 32768, bit k of the state has
# period 2^(k+1), so the low bits cycle almost immediately. Every value below
# is a small modulo of the state, which is exactly what reads those bits:
# ($seed / 8) % 4 repeats every 32 draws with 32768, and does not with 32749.
VAR $seed = $SECOND * 137 + $COUNTER * 2749 + $MINUTE * 31
LET $seed = $seed % 32749
VAR $rnd = 0

VAR $factor = 0
VAR $half = 0
VAR $span_x = 0
VAR $span_y = 0
VAR $cx = 0
VAR $cy = 0
VAR $ox = 0 - 10
VAR $oy = 0 - 10
VAR $eye_type = 0
VAR $rotation = 0

COLOR NAME=BLACK

REPEAT COUNT=$num_eyes

  # --- size first: everything else depends on it ---
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $factor = $rnd % $factor_range + $min_factor

  # Margin from the edge. The eye turns about its own centre through a
  # full circle, so the reach is the half-diagonal of the *inked* part of
  # the pattern (20 wide, ~15 tall), not half its width: 13 per scale step.
  LET $half = $factor * 13

  # --- centre, kept fully on screen for this size and any angle ---
  LET $span_x = $WIDTH - $factor * 26
  LET $span_y = $HEIGHT - $factor * 26
  IF $span_x < 1 THEN
    LET $span_x = 1
  ENDIF
  IF $span_y < 1 THEN
    LET $span_y = 1
  ENDIF

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cx = $rnd % $span_x + $half

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cy = $rnd % $span_y + $half

  # --- which eye ---
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $eye_type = $rnd % 4 + 1

  # --- any angle, full circle ---
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $rotation = $rnd % 360

  # --- draw: translate to the centre, then draw the pattern centred on it.
  # Order matters. Points go Scale -> Rotate -> Translate, so the
  # DRAW coordinates are multiplied by the scale factor: pass -10,-10
  # (half a pattern) and the eye lands centred on the translation.
  RESET_TRANSFORMS
  TRANSLATE DX=$cx DY=$cy
  ROTATE DEGREES=$rotation
  SCALE FACTOR=$factor

  IF $eye_type == 1 THEN
    DRAW NAME="eye1" X=$ox Y=$oy
  ENDIF
  IF $eye_type == 2 THEN
    DRAW NAME="eye2" X=$ox Y=$oy
  ENDIF
  IF $eye_type == 3 THEN
    DRAW NAME="eye3" X=$ox Y=$oy
  ENDIF
  IF $eye_type == 4 THEN
    DRAW NAME="eye4" X=$ox Y=$oy
  ENDIF

ENDREPEAT
RESET_TRANSFORMS
