# =====================================================================
# Seascape II -- the same sea, painted instead of drawn
#
# Where seascape.mp is a poster, this is a canvas: nothing has a clean
# edge. The sun is a ragged core with rays, the clouds are torn heaps
# of dabs, the sky and sea are hundreds of short strokes over a tonal
# ground, the horizon is a broken chain of dabs.
#
# THE CLOCK REPAINTS THE PICTURE
#     $HOUR    -> the whole palette. Day: white sky, light sea, a sun
#                 with many rays. Dusk/dawn: toned sky. Night: grey
#                 sky, near-black sea, few rays. Sun height too.
#     $HOUR    -> tide. Low tide around hours 0,1,11,12,13,23 exposes
#                 black rocks with foam at the bottom of the sea; the
#                 lower the tide, the bigger and more numerous.
#     $MINUTE  -> sun travels left to right; more strokes on the water.
#     $COUNTER -> clouds drift, and every stroke, dab, tear and rock
#                 is re-jittered, so clouds shred differently each run.
#     $SECOND  -> extra shimmer.
#
# BOTH PANELS
#   All lengths derive from $WIDTH/$HEIGHT; round things from the
#   smaller dimension; stroke counts from the area they cover.
# =====================================================================

DEFINE PATTERN NAME="d1" WIDTH=4 HEIGHT=4 DATA="1000000000000000"
DEFINE PATTERN NAME="d2" WIDTH=4 HEIGHT=4 DATA="1000000000100000"
DEFINE PATTERN NAME="d4" WIDTH=4 HEIGHT=4 DATA="1010000001010000"
DEFINE PATTERN NAME="d8" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="d12" WIDTH=4 HEIGHT=4 DATA="0101111110101111"
DEFINE PATTERN NAME="d14" WIDTH=4 HEIGHT=4 DATA="0111111111011111"

# ---- layout --------------------------------------------------------
VAR $minwh = $WIDTH
IF $HEIGHT < $WIDTH THEN
  LET $minwh = $HEIGHT
ENDIF
VAR $horizon = $HEIGHT * 5 / 9
VAR $sea_h = $HEIGHT - $horizon
VAR $sun_r = $minwh / 8
IF $sun_r < 10 THEN
  LET $sun_r = 10
ENDIF
VAR $half_r = $sun_r / 2
VAR $hline = $HEIGHT / 240 + 1

# ---- time of day ---------------------------------------------------
VAR $noon_d = $HOUR - 12
IF $noon_d < 0 THEN
  LET $noon_d = 0 - $noon_d
ENDIF
# 0 = day, 1 = dusk/dawn, 2 = night
VAR $sky_mood = 0
IF $noon_d >= 7 THEN
  LET $sky_mood = 1
ENDIF
IF $noon_d >= 10 THEN
  LET $sky_mood = 2
ENDIF
# 0..3, the sea darkens as the sun sinks
VAR $sea_tone = 0
IF $noon_d >= 4 THEN
  LET $sea_tone = 1
ENDIF
IF $noon_d >= 7 THEN
  LET $sea_tone = 2
ENDIF
IF $noon_d >= 10 THEN
  LET $sea_tone = 3
ENDIF

# ---- tide ----------------------------------------------------------
# A 12-hour cycle: |($HOUR % 12) - 6| runs 0..6, high water at 6.
# Exposure > 0 only in the lowest two steps of the cycle.
VAR $tide_a = $HOUR % 12 - 6
IF $tide_a < 0 THEN
  LET $tide_a = 0 - $tide_a
ENDIF
VAR $exposure = $tide_a - 4
IF $exposure < 0 THEN
  LET $exposure = 0
ENDIF

# ---- sun position --------------------------------------------------
VAR $sx_min = $sun_r + 2
VAR $sx_span = $WIDTH - $sun_r * 2 - 4
VAR $sun_x = $MINUTE * $sx_span / 59 + $sx_min
VAR $sun_top = $sun_r + $sun_r / 2
VAR $sun_low = $horizon - $sun_r / 3
VAR $sun_span = $sun_low - $sun_top
VAR $sun_y = $noon_d * $sun_span / 12 + $sun_top

# ---- rng (see random.mp) -------------------------------------------
VAR $seed = $SECOND * 137 + $COUNTER * 2749 + $MINUTE * 31 + $HOUR * 907
LET $seed = $seed % 32749
VAR $rnd = 0
REPEAT COUNT=4
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
ENDREPEAT

# ---- ground --------------------------------------------------------
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

# sky ground: white by day, barely toned at dusk, light grey at
# night -- the sky stays luminous, the dark lives in the sea
COLOR NAME=BLACK
IF $sky_mood == 1 THEN
  FILL NAME="d2"
  FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$horizon
ENDIF
IF $sky_mood == 2 THEN
  FILL NAME="d4"
  FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$horizon
ENDIF

# (the sea ground is painted AFTER the sun and clouds, so a sun that
# has sunk below the horizon is covered by the water, not glowing
# through it)

# ---- sky strokes: short slanted lines, denser toward the top -------
FILL NAME=SOLID
VAR $sky_n = $WIDTH * $horizon / 1300 + 12
VAR $st_h = $minwh / 270 + 1
VAR $st_len = 0
VAR $st_x = 0
VAR $st_y = 0
VAR $st_y2 = 0
VAR $st_x2 = 0
REPEAT COUNT=$sky_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $st_y = $rnd % $horizon
  LET $st_y = $st_y * $st_y / $horizon

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $st_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $st_len = $rnd % $sun_r + $minwh / 20

  LET $st_x2 = $st_x + $st_len
  LET $st_y2 = $st_y + $rnd % 5 - 2

  COLOR NAME=BLACK
  IF $INDEX % 6 == 0 THEN
    COLOR NAME=WHITE
  ENDIF
  LINE X1=$st_x Y1=$st_y X2=$st_x2 Y2=$st_y2
ENDREPEAT

# ---- sun: glow, white rays, ragged core ----------------------------
# A wide dithered glow goes down first; the rays are WHITE strokes
# carved through it, so the sun radiates light instead of black
# spokes. Ray count falls with the sun: 16 blazing at noon, 4 at
# midnight.
VAR $rays = 16 - $noon_d
IF $rays < 4 THEN
  LET $rays = 4
ENDIF
VAR $ray_step = 360 / $rays
VAR $ray_r1 = $sun_r * 2 / 3
VAR $ray_r2 = 0
VAR $ray_len = 0
VAR $ray_h = $minwh / 200 + 1
VAR $ray_y0 = 0 - $ray_h / 2
VAR $halo_r = $sun_r * 5 / 3
TRANSLATE DX=$sun_x DY=$sun_y
COLOR NAME=BLACK
FILL NAME="d4"
FILL_CIRCLE X=0 Y=0 RADIUS=$halo_r
COLOR NAME=WHITE
FILL NAME=SOLID
REPEAT COUNT=$rays
  ROTATE DEGREES=$ray_step
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $ray_r2 = $halo_r + $rnd % $half_r
  LET $ray_len = $ray_r2 - $ray_r1
  FILL_RECT X=$ray_r1 Y=$ray_y0 WIDTH=$ray_len HEIGHT=$ray_h
ENDREPEAT

# the white core, then dabs stumbling around the rim
COLOR NAME=WHITE
FILL NAME=SOLID
VAR $core_r = $sun_r * 9 / 10
FILL_CIRCLE X=0 Y=0 RADIUS=$core_r
VAR $dab_r = $minwh / 120 + 2
VAR $rim_r = 0
VAR $rim_jit = $sun_r / 3
IF $rim_jit < 2 THEN
  LET $rim_jit = 2
ENDIF
REPEAT COUNT=20
  ROTATE DEGREES=18
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $rim_r = $sun_r - $rim_jit / 2 + $rnd % $rim_jit
  COLOR NAME=WHITE
  FILL_CIRCLE X=$rim_r Y=0 RADIUS=$dab_r
ENDREPEAT
RESET_TRANSFORMS

# ---- clouds: torn heaps of dabs ------------------------------------
# Base centres are evenly spaced and drift with $COUNTER, but the body
# of each cloud is 6..17 fragments thrown around the centre, so the
# same cloud shreds differently every run. Every third fragment drags
# a small black shadow under itself.
VAR $n_clouds = $HOUR % 3 + 2
VAR $spacing = $WIDTH / $n_clouds
VAR $drift = $COUNTER % 24 * $WIDTH / 24
VAR $cy_base = $horizon / 8
VAR $cy_span = $horizon / 3
IF $cy_span < 1 THEN
  LET $cy_span = 1
ENDIF
VAR $cr_base = $minwh / 16
VAR $cr_span = $minwh / 18
IF $cr_span < 1 THEN
  LET $cr_span = 1
ENDIF
VAR $cx = 0
VAR $cy = 0
VAR $cr = 0
VAR $c3 = 0
VAR $c3h = 0
VAR $crh = 0
VAR $frags = 0
VAR $fx = 0
VAR $fy = 0
VAR $fr = 0
VAR $fshadow = 0
FILL NAME=SOLID
REPEAT COUNT=$n_clouds
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cy = $rnd % $cy_span + $cy_base

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cr = $rnd % $cr_span + $cr_base

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $frags = $rnd % 12 + 6

  LET $cx = $INDEX * $spacing + $spacing / 2 + $drift
  LET $cx = $cx % $WIDTH
  LET $c3 = $cr * 3
  LET $c3h = $cr * 3 / 2
  LET $crh = $cr / 2
  IF $crh < 1 THEN
    LET $crh = 1
  ENDIF

  REPEAT COUNT=$frags
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $fx = $rnd % $c3 - $c3h + $cx

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $fy = $rnd % $cr - $crh + $cy

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $fr = $rnd % $crh + $cr / 5 + 1

    # a cloud is one dither step darker than its sky, never white:
    # texture against the ground, and the sun stays the only white
    COLOR NAME=BLACK
    IF $sky_mood == 0 THEN
      FILL NAME="d2"
    ENDIF
    IF $sky_mood == 1 THEN
      FILL NAME="d4"
    ENDIF
    IF $sky_mood == 2 THEN
      FILL NAME="d8"
    ENDIF
    FILL_CIRCLE X=$fx Y=$fy RADIUS=$fr
    FILL NAME=SOLID
    # every third fragment drags a soft dithered shadow, not a black dot
    IF $INDEX % 3 == 0 THEN
      COLOR NAME=BLACK
      FILL NAME="d8"
      LET $fshadow = $fy + $fr
      LET $fr = $fr / 2 + 1
      FILL_CIRCLE X=$fx Y=$fshadow RADIUS=$fr
      FILL NAME=SOLID
    ENDIF
  ENDREPEAT
ENDREPEAT

# ---- sea ground, painted over whatever sank below the horizon -----
COLOR NAME=BLACK
FILL NAME="d4"
IF $sea_tone == 1 THEN
  FILL NAME="d8"
ENDIF
IF $sea_tone == 2 THEN
  FILL NAME="d12"
ENDIF
IF $sea_tone == 3 THEN
  FILL NAME="d14"
ENDIF
FILL_RECT X=0 Y=$horizon WIDTH=$WIDTH HEIGHT=$sea_h

# ---- horizon: no line at all, a noised frontier --------------------
# Two passes of dabs in the two GROUND fills. Sea-ground dabs reach up
# into the sky, sky-ground dabs bite down into the water, so the
# boundary is an interpenetration, not an edge. Because pattern fills
# tile in screen space, each dab continues its own ground seamlessly.
VAR $hz_amp = $minwh / 40 + 3
VAR $hz_n = $WIDTH / 4 + 8
VAR $hz_x = 0
VAR $hz_y = 0
VAR $hz_w = 0
VAR $hz_h = 0

# sea into sky
COLOR NAME=BLACK
FILL NAME="d4"
IF $sea_tone == 1 THEN
  FILL NAME="d8"
ENDIF
IF $sea_tone == 2 THEN
  FILL NAME="d12"
ENDIF
IF $sea_tone == 3 THEN
  FILL NAME="d14"
ENDIF
REPEAT COUNT=$hz_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $hz_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $hz_h = $rnd % $hz_amp + 1
  LET $hz_y = $horizon - $hz_h
  LET $hz_w = $rnd % $hz_amp + 3
  FILL_RECT X=$hz_x Y=$hz_y WIDTH=$hz_w HEIGHT=$hz_h
ENDREPEAT

# sky into sea, in whatever the sky's ground is for this mood.
# Half as many dabs, and smaller: light ink on the dark water merges
# into a pale band if it is laid as thickly as the dark ink above.
# The anchor also jitters a little, so the bites start at slightly
# different depths instead of hanging off one straight edge.
IF $sky_mood == 0 THEN
  COLOR NAME=WHITE
  FILL NAME=SOLID
ENDIF
IF $sky_mood == 1 THEN
  COLOR NAME=BLACK
  FILL NAME="d2"
ENDIF
IF $sky_mood == 2 THEN
  COLOR NAME=BLACK
  FILL NAME="d4"
ENDIF
VAR $hz_n2 = $hz_n / 2
VAR $hz_amp2 = $hz_amp * 2 / 3
IF $hz_amp2 < 2 THEN
  LET $hz_amp2 = 2
ENDIF
REPEAT COUNT=$hz_n2
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $hz_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $hz_h = $rnd % $hz_amp2 + 2
  LET $hz_w = $rnd % $hz_amp2 + 3
  LET $hz_y = $horizon - 2 + $rnd % 4
  FILL_RECT X=$hz_x Y=$hz_y WIDTH=$hz_w HEIGHT=$hz_h
ENDREPEAT
FILL NAME=SOLID

# ---- sea strokes: longer and thicker toward the viewer -------------
VAR $sea_n = $WIDTH * $sea_h / 700 + $MINUTE + $SECOND / 4
VAR $depth = 0
REPEAT COUNT=$sea_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $st_y = $rnd % $sea_h + $horizon

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $st_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $depth = $st_y - $horizon
  LET $st_len = $rnd % $sun_r + $minwh / 24
  LET $st_len = $depth * $WIDTH / $sea_h / 8 + $st_len

  IF $sea_tone == 0 THEN
    COLOR NAME=BLACK
  ELSE
    COLOR NAME=WHITE
  ENDIF
  IF $INDEX % 5 == 0 THEN
    IF $sea_tone == 0 THEN
      COLOR NAME=WHITE
    ELSE
      COLOR NAME=BLACK
    ENDIF
  ENDIF
  LET $st_y2 = $st_y + $rnd % 3 - 1
  LET $st_x2 = $st_x + $st_len
  LINE X1=$st_x Y1=$st_y X2=$st_x2 Y2=$st_y2
ENDREPEAT

# ---- reflection: a troubled column of dabs under the sun -----------
# Its length follows the sun: a high noon sun leaves a short glitter
# near the horizon, a sinking sun stretches it toward the viewer.
# Dashes wander sideways, ride up and down, and sometimes break off.
VAR $refl_step = $HEIGHT / 64
IF $refl_step < 3 THEN
  LET $refl_step = 3
ENDIF
VAR $dash_h = $refl_step / 4
IF $dash_h < 1 THEN
  LET $dash_h = 1
ENDIF
VAR $refl_len = $sea_h / 4 + $noon_d * $sea_h / 20
IF $refl_len > $sea_h THEN
  LET $refl_len = $sea_h
ENDIF
VAR $refl_count = $refl_len / $refl_step
VAR $rw = 0
VAR $rx = 0
VAR $ry = 0
VAR $sun_r2 = $sun_r * 2
COLOR NAME=WHITE
FILL NAME=SOLID
REPEAT COUNT=$refl_count
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $rw = $rnd % $sun_r + $sun_r / 3

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $rx = $rnd % $sun_r2 - $sun_r + $sun_x - $rw / 2

  LET $ry = $INDEX * $refl_step + $horizon + $hline + 2
  LET $ry = $ry + $rnd % 5 - 2
  IF $rnd % 7 != 0 THEN
    FILL_RECT X=$rx Y=$ry WIDTH=$rw HEIGHT=$dash_h
  ENDIF
ENDREPEAT

# ---- sunset pillar -------------------------------------------------
# When the disc touches the water, the thin glitter is not enough:
# a bright column pours straight down from the point of contact --
# wide at the top, tapering, torn at the edges, with pieces missing.
VAR $sun_bot = $sun_y + $sun_r
VAR $touch_y = $horizon - $sun_r / 3
IF $sun_bot >= $touch_y THEN
  VAR $glow_step = $refl_step / 2
  IF $glow_step < 2 THEN
    LET $glow_step = 2
  ENDIF
  VAR $glow_n = $sea_h / 3 / $glow_step
  VAR $gw = 0
  VAR $gx = 0
  VAR $gy = 0
  VAR $g_h = $dash_h + 1
  COLOR NAME=WHITE
  REPEAT COUNT=$glow_n
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $gw = $glow_n - $INDEX
    LET $gw = $sun_r * 2 * $gw / $glow_n
    LET $gw = $gw + $rnd % $half_r - $sun_r / 4
    IF $gw >= 3 THEN
      LET $gx = $rnd % $half_r - $sun_r / 4 + $sun_x - $gw / 2
      LET $gy = $INDEX * $glow_step + $horizon + 1
      LET $gy = $gy + $rnd % 3 - 1
      IF $rnd % 6 != 0 THEN
        FILL_RECT X=$gx Y=$gy WIDTH=$gw HEIGHT=$g_h
      ENDIF
    ENDIF
  ENDREPEAT
ENDIF

# ---- rocks, when the tide is out -----------------------------------
# Black clusters low in the water, foam dashes at the waterline and a
# white glint on top so they read even on a near-black night sea.
IF $exposure > 0 THEN
  # Few of them, but massive, scattered over a third of the sea's
  # depth and each a different bulk -- some squat, some stacked tall.
  VAR $rock_n = $exposure + 1
  VAR $rock_x = 0
  VAR $rock_y = 0
  VAR $rock_r = 0
  VAR $rk = 0
  VAR $rock_span = $sea_h / 3 + 1
  VAR $rock_base = $HEIGHT - $sea_h / 6
  VAR $rock_rspan = $minwh / 20 + 1
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $rock_n = $rnd % 2 + $rock_n
  VAR $rk_sp = $WIDTH / $rock_n
  IF $rk_sp < 1 THEN
    LET $rk_sp = 1
  ENDIF
  REPEAT COUNT=$rock_n
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_x = $INDEX * $rk_sp + $rnd % $rk_sp

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_y = $rock_base - $rnd % $rock_span

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_r = $rnd % $rock_rspan + $exposure * $minwh / 70 + $minwh / 28

    COLOR NAME=BLACK
    FILL_CIRCLE X=$rock_x Y=$rock_y RADIUS=$rock_r
    IF $rnd % 2 == 0 THEN
      LET $rk = $rock_y - $rock_r
      LET $rock_rspan = $rock_r * 2 / 3
      FILL_CIRCLE X=$rock_x Y=$rk RADIUS=$rock_rspan
      LET $rock_rspan = $minwh / 20 + 1
    ENDIF
    LET $rk = $rock_x - $rock_r
    LET $rock_y = $rock_y + $rock_r / 4
    LET $rock_r = $rock_r * 2 / 3
    FILL_CIRCLE X=$rk Y=$rock_y RADIUS=$rock_r
    LET $rk = $rock_x + $rock_r + $rock_r / 2
    FILL_CIRCLE X=$rk Y=$rock_y RADIUS=$rock_r

    COLOR NAME=WHITE
    LET $rk = $rock_x - $rock_r * 2
    FILL_RECT X=$rk Y=$rock_y WIDTH=$rock_r HEIGHT=$hline
    LET $rk = $rock_x + $rock_r * 2
    FILL_RECT X=$rk Y=$rock_y WIDTH=$rock_r HEIGHT=$hline
    LET $rk = $rock_x - $rock_r / 2
    LET $rock_y = $rock_y - $rock_r - $rock_r / 2
    FILL_RECT X=$rk Y=$rock_y WIDTH=$rock_r HEIGHT=$hline
  ENDREPEAT
ENDIF
