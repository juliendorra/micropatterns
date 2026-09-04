# =====================================================================
# Seascape III -- the sea as a woodblock print, cut entirely from
# pattern blocks
#
# Every figure on this print is one of SIXTEEN patterns (the language
# maximum), stamped with DRAW under the full transform state and
# altered on the paper rather than in the data:
#   scaled     -- the same wave block prints at 1x on the horizon and
#                 6x in the foreground; the sun is a 20px block blown
#                 up to its final size
#   rotated    -- rays revolve around the sun, stars twist 45 degrees,
#                 every rock leans a few degrees off plumb
#   overlapped -- a rock is five stamps of one block: four in the
#                 paper's ink, offset one unit each way, make its
#                 contour; the fifth in figure ink is the rock
#   dithered   -- the halo block is a radial dither; stamped over a
#                 mist band's end it frays it, stamped along the far
#                 wave rows it fades them into the distance, stamped
#                 under a glitter dash it clears fuzzy water for it
#   filled     -- the grounds and the dusk sky still use the dither
#                 blocks as FILL patterns, tiled in screen space
#
# THE CLOCK CUTS A DIFFERENT BLOCK
#     $HOUR    -> palette (day ink on paper, dusk toned, night fully
#                 inverted with stars), sun height, ray count, number
#                 of mist bands and ink clouds; tide -- hours
#                 0,1,11,12,13,23 uncover leaning sea stacks
#     $MINUTE  -> sun slides left to right
#     $COUNTER -> the swell drifts with parallax; mist travels
#     $SECOND  -> a few more stars at night
#
# BOTH PANELS: wave rows scale from the sea's depth, the sun from the
# smaller dimension; stamp counts derive from what they must cover.
# =====================================================================

DEFINE PATTERN NAME="wave" WIDTH=20 HEIGHT=20 DATA="0000000011011100000000001100000000110000000100000110000010000001001111111100010001110100000001100010010000010110000100101000011011111001100010011000000011001000000100001000010000000011010000000110010000000000001100100110100010010110000000111101101000000100000000010010000001100100000010001000000000001000000010010000000111100110011001100111000000100000010000000001001000000100000000011011000010011000"
DEFINE PATTERN NAME="sun20" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000011111111000000000011111111111100000001111000000111100000111100111100011100001111111111110011000111011111011110111001100111110111100110011011110000111111100110110000001111011001101111000000110110011011110000111101100110011110111110011001110111101111101110001100111111110011000011100111110001110000011111100001111000000011111111111100000000001111111100000000000000000000000000"
DEFINE PATTERN NAME="halo20" WIDTH=20 HEIGHT=20 DATA="0000000100000000000000000000010000000000000010000001000000000000000000000000000000000010101010110000000100000101000001000000101010101010100010000001010101000000001010101010101010000000010101010100001000001010101010101000010001010101010000000000101010101010110000000001010101000000000010101010101000000010000000000000100000001010101010000000000000100000010000000000000010000000000000000000001000000000"
DEFINE PATTERN NAME="mist20" WIDTH=20 HEIGHT=8 DATA="0000000000000000000010111110101101110111110110111110111101010110111101111011111000000000000000000000000000000000000000000000111110111011000000000000000000000000"
DEFINE PATTERN NAME="cloud20" WIDTH=20 HEIGHT=12 DATA="000000000000000000000000000000000000000000000000001101110000000001100111111110000000110111011011111000011111011111111010011010111110110111101111111110111111111110110111111110101111011111111101111110000011011101111011000000000000000111000000"
DEFINE PATTERN NAME="rock20" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000000000000000000000000000011000000000000001111111000000000000110000111000000000001111110111000000001111111111010000000011100011111111000001101110111110110000011100100011111100000100111010011111100010011111100011111000111100011110011110000110011111111011100110001111111111101101101000111111111111011111111001111100110101111111000111101011111111111101111111"
DEFINE PATTERN NAME="foam20" WIDTH=20 HEIGHT=8 DATA="0000000000000000000000000000000000000000000000000000000000001111110111111011111110000111000011100001000001110000111000000000110100001011000000001001100110010000"
DEFINE PATTERN NAME="glint20" WIDTH=20 HEIGHT=6 DATA="001101111100000000000000000000000000000000000000111111011100000000000000000000001101110000001111011100000000000000000000"
DEFINE PATTERN NAME="star5" WIDTH=5 HEIGHT=5 DATA="0010000100111110010000100"
DEFINE PATTERN NAME="bird20" WIDTH=20 HEIGHT=6 DATA="000000000000000000000000000000000000000000000000000000000000011000110000101000000001010000110101100000001000000000000000"
DEFINE PATTERN NAME="ray16" WIDTH=16 HEIGHT=4 DATA="1111110000000000111111111100000011111111111111111111000000000000"
DEFINE PATTERN NAME="d1" WIDTH=4 HEIGHT=4 DATA="1000000000000000"
DEFINE PATTERN NAME="d2" WIDTH=4 HEIGHT=4 DATA="1000000000100000"
DEFINE PATTERN NAME="d4" WIDTH=4 HEIGHT=4 DATA="1010000001010000"
DEFINE PATTERN NAME="d8" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="d12" WIDTH=4 HEIGHT=4 DATA="0101111110101111"

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
VAR $hline = $HEIGHT / 240 + 1

# how much sky there is to fill, in units of the 200x200 panel's sky:
# 0 on the watch, ~7 on either M5Paper orientation. Element counts
# grow with it so the big print's sky is as occupied as the small one.
VAR $sky_fac = $WIDTH * $horizon / 40000

# ---- time of day ---------------------------------------------------
VAR $noon_d = $HOUR - 12
IF $noon_d < 0 THEN
  LET $noon_d = 0 - $noon_d
ENDIF
VAR $mood = 0
IF $noon_d >= 7 THEN
  LET $mood = 1
ENDIF
IF $noon_d >= 10 THEN
  LET $mood = 2
ENDIF

# ---- tide ----------------------------------------------------------
VAR $tide_a = $HOUR % 12 - 6
IF $tide_a < 0 THEN
  LET $tide_a = 0 - $tide_a
ENDIF
VAR $exposure = $tide_a - 4
IF $exposure < 0 THEN
  LET $exposure = 0
ENDIF

# ---- sun position --------------------------------------------------
VAR $sx_span = $WIDTH - $sun_r * 2 - 4
VAR $sun_x = $MINUTE * $sx_span / 59 + $sun_r + 2
VAR $sun_top = $sun_r + $sun_r / 2
VAR $sun_low = $horizon - $sun_r / 3
VAR $sun_span = $sun_low - $sun_top
VAR $sun_y = $noon_d * $sun_span / 12 + $sun_top

# ---- stamp scales, all from the panel ------------------------------
# sun20 is 20 units across; scale it so the printed disc is 2*$sun_r
VAR $s_sun = $sun_r / 10
IF $s_sun < 1 THEN
  LET $s_sun = 1
ENDIF
VAR $st_s = $minwh / 300 + 1
VAR $m_s = $minwh / 400 + 1
VAR $c_s = $minwh / 150 + 1
VAR $b_s = $minwh / 300 + 1
VAR $g_s = $s_sun / 2
IF $g_s < 1 THEN
  LET $g_s = 1
ENDIF

# ---- rng (see random.mp) -- a print is deliberate, so only jitter --
VAR $seed = $SECOND * 137 + $COUNTER * 2749 + $MINUTE * 31 + $HOUR * 907
LET $seed = $seed % 32749
VAR $rnd = 0
REPEAT COUNT=4
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
ENDREPEAT

# ---- paper ---------------------------------------------------------
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

# sky: white by day, a d4 FILL at dusk, black at night
COLOR NAME=BLACK
IF $mood == 1 THEN
  FILL NAME="d4"
  FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$horizon
  FILL NAME=SOLID
ENDIF
IF $mood == 2 THEN
  FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$horizon
ENDIF

# ---- stars, night only: the star block, every other one twisted ----
VAR $star_n = $SECOND / 4 + 12 + $sky_fac * 4
VAR $star_x = 0
VAR $star_y = 0
VAR $star_sky = $horizon * 2 / 3
VAR $star_rot = 0
IF $mood == 2 THEN
  COLOR NAME=WHITE
  REPEAT COUNT=$star_n
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $star_x = $rnd % $WIDTH

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $star_y = $rnd % $star_sky

    LET $star_rot = $INDEX % 2 * 45
    RESET_TRANSFORMS
    TRANSLATE DX=$star_x DY=$star_y
    SCALE FACTOR=$st_s
    ROTATE DEGREES=$star_rot
    DRAW NAME="star5" X=-2 Y=-2
  ENDREPEAT
  RESET_TRANSFORMS
ENDIF

# ---- birds, day only: the bird block, tilted a touch ---------------
VAR $bird_n = 0
VAR $bird_x = 0
VAR $bird_y = 0
VAR $bird_rot = 0
IF $mood == 0 THEN
  LET $bird_n = $HOUR % 3 + 1 + $sky_fac / 3
  COLOR NAME=BLACK
  REPEAT COUNT=$bird_n
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $bird_x = $rnd % $WIDTH

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $bird_y = $rnd % $star_sky + 4
    LET $bird_rot = $rnd % 17 + 351
    LET $bird_rot = $bird_rot % 360

    RESET_TRANSFORMS
    TRANSLATE DX=$bird_x DY=$bird_y
    SCALE FACTOR=$b_s
    ROTATE DEGREES=$bird_rot
    DRAW NAME="bird20" X=-10 Y=-3
  ENDREPEAT
  RESET_TRANSFORMS
ENDIF

# ---- sun: halo block at double scale, ray block revolved, disc -----
VAR $rays = 12 - $noon_d
IF $rays < 4 THEN
  LET $rays = 4
ENDIF
VAR $ray_step = 360 / $rays
VAR $s_halo = $s_sun * 2
IF $mood == 2 THEN
  COLOR NAME=WHITE
ELSE
  COLOR NAME=BLACK
ENDIF
RESET_TRANSFORMS
TRANSLATE DX=$sun_x DY=$sun_y
SCALE FACTOR=$s_halo
DRAW NAME="halo20" X=-10 Y=-10
# rays print at half the sun's scale -- slimmer cuts -- and start
# just past the halo's rim (40 half-scale units = the halo radius)
SCALE FACTOR=$g_s
REPEAT COUNT=$rays
  ROTATE DEGREES=$ray_step
  DRAW NAME="ray16" X=38 Y=-2
ENDREPEAT
RESET_TRANSFORMS
TRANSLATE DX=$sun_x DY=$sun_y
SCALE FACTOR=$s_sun
DRAW NAME="sun20" X=-10 Y=-10
RESET_TRANSFORMS

# ---- ink clouds: the torn cloud block, scaled and leaned -----------
VAR $cloud_n = $HOUR % 2 + 1 + $sky_fac / 3
VAR $cl_x = 0
VAR $cl_y = 0
VAR $cl_rot = 0
IF $mood == 2 THEN
  COLOR NAME=WHITE
ELSE
  COLOR NAME=BLACK
ENDIF
REPEAT COUNT=$cloud_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cl_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $cl_y = $rnd % $star_sky + 6
  LET $cl_rot = $rnd % 13 + 353
  LET $cl_rot = $cl_rot % 360

  RESET_TRANSFORMS
  TRANSLATE DX=$cl_x DY=$cl_y
  SCALE FACTOR=$c_s
  ROTATE DEGREES=$cl_rot
  DRAW NAME="cloud20" X=-10 Y=-6
ENDREPEAT
RESET_TRANSFORMS

# ---- mist bands: runs of the mist block, ends frayed by the halo ---
# The halo block does double duty here: stamped past the left end in
# band ink it frays the band outward; stamped ON the right end in the
# paper's ink it eats the band away. Overlap as erosion.
VAR $mist_n = $HOUR % 3 + 2 + $sky_fac / 2
VAR $mist_sp = $WIDTH / $mist_n
VAR $mist_drift = $COUNTER % 24 * $WIDTH / 24
VAR $mist_x = 0
VAR $mist_y = 0
VAR $mist_t = 0
VAR $mist_u = 0
VAR $mist_jit = $horizon / 8
IF $mist_jit < 1 THEN
  LET $mist_jit = 1
ENDIF
REPEAT COUNT=$mist_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  # spread the bands over the sky whatever their number
  LET $mist_y = $INDEX * $horizon * 3 / 4 / $mist_n + $horizon / 8 + $rnd % $mist_jit

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $mist_t = $rnd % 3 + 2 + $WIDTH / 300

  LET $mist_x = $INDEX * $mist_sp + $mist_drift
  LET $mist_x = $mist_x % $WIDTH

  RESET_TRANSFORMS
  TRANSLATE DX=$mist_x DY=$mist_y
  SCALE FACTOR=$m_s
  IF $mood == 2 THEN
    COLOR NAME=WHITE
  ELSE
    COLOR NAME=BLACK
  ENDIF
  REPEAT COUNT=$mist_t
    LET $mist_u = $INDEX * 20
    DRAW NAME="mist20" X=$mist_u Y=0
  ENDREPEAT
  DRAW NAME="halo20" X=-14 Y=-6
  IF $mood == 2 THEN
    COLOR NAME=BLACK
  ELSE
    COLOR NAME=WHITE
  ENDIF
  LET $mist_u = $mist_t * 20 - 8
  DRAW NAME="halo20" X=$mist_u Y=-6
ENDREPEAT
RESET_TRANSFORMS

# ---- water ground, AFTER the sun so a sunk disc is covered ---------
IF $mood == 2 THEN
  COLOR NAME=BLACK
ELSE
  COLOR NAME=WHITE
ENDIF
FILL NAME=SOLID
FILL_RECT X=0 Y=$horizon WIDTH=$WIDTH HEIGHT=$sea_h

# ---- the swell: wave block rows, far rows faded by halo stamps -----
VAR $s_cap = $sea_h / 60 + 1
IF $s_cap > 6 THEN
  LET $s_cap = 6
ENDIF
VAR $row_y = $horizon
VAR $s = 1
VAR $tile_w = 0
VAR $cols = 0
VAR $stag_u = 0
VAR $drift_u = $COUNTER % 20
VAR $u = 0
VAR $jy = 0
VAR $row_i = 0
REPEAT COUNT=12
  IF $row_y < $HEIGHT THEN
    LET $row_i = $INDEX
    LET $s = $INDEX / 2 + 1
    IF $s > $s_cap THEN
      LET $s = $s_cap
    ENDIF
    LET $tile_w = 20 * $s
    LET $cols = $WIDTH / $tile_w + 4
    LET $stag_u = $INDEX % 2 * 10
    RESET_TRANSFORMS
    TRANSLATE DX=0 DY=$row_y
    SCALE FACTOR=$s
    IF $mood == 2 THEN
      COLOR NAME=WHITE
    ELSE
      COLOR NAME=BLACK
    ENDIF
    REPEAT COUNT=$cols
      LET $u = $INDEX * 20 - 40 - $stag_u - $drift_u
      # the two far rows ride up and down a few units per tile, so
      # the frontier is a broken crest, not a rule
      LET $jy = 0
      IF $row_i < 2 THEN
        LET $seed = $seed * 2053 + 13849
        LET $seed = $seed % 32749
        LET $rnd = $seed / 8
        LET $jy = $rnd % 7 - 3
      ENDIF
      DRAW NAME="wave" X=$u Y=$jy
    ENDREPEAT
    # atmospheric fade: erode the two far rows with the halo dither,
    # stamped in the paper's ink over every tile
    IF $row_i < 2 THEN
      IF $mood == 2 THEN
        COLOR NAME=BLACK
      ELSE
        COLOR NAME=WHITE
      ENDIF
      REPEAT COUNT=$cols
        LET $u = $INDEX * 20 - 33 - $stag_u - $drift_u
        DRAW NAME="halo20" X=$u Y=0
      ENDREPEAT
    ENDIF
    RESET_TRANSFORMS
    LET $row_y = $row_y + $tile_w
  ENDIF
ENDREPEAT
RESET_TRANSFORMS

# no horizon line: a spray of halo stamps straddles the frontier,
# alternating wave ink (spatter thrown above the crest) and the
# water's ground ink (dithered bites into the first row of swell)
VAR $sp_n = $WIDTH / 40 + 3
VAR $sp_x = 0
VAR $sp_y = 0
VAR $sp_s = $minwh / 400 + 1
REPEAT COUNT=$sp_n
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $sp_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $sp_y = $horizon + $rnd % 16 - 11

  RESET_TRANSFORMS
  TRANSLATE DX=$sp_x DY=$sp_y
  SCALE FACTOR=$sp_s
  IF $INDEX % 2 == 0 THEN
    IF $mood == 2 THEN
      COLOR NAME=WHITE
    ELSE
      COLOR NAME=BLACK
    ENDIF
  ELSE
    IF $mood == 2 THEN
      COLOR NAME=BLACK
    ELSE
      COLOR NAME=WHITE
    ENDIF
  ENDIF
  DRAW NAME="halo20" X=-10 Y=-10
ENDREPEAT
RESET_TRANSFORMS

# the same spray, sparser, scattered over the open water: mostly
# ground-ink stamps (patches of foam eaten out of the swell), every
# third one in wave ink (a darker clump of chop)
VAR $sea_sp = $WIDTH * $sea_h / 25000 + 3
VAR $sea_span = $sea_h - 12
IF $sea_span < 1 THEN
  LET $sea_span = 1
ENDIF
REPEAT COUNT=$sea_sp
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $sp_x = $rnd % $WIDTH

  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $sp_y = $rnd % $sea_span + $horizon + 8

  RESET_TRANSFORMS
  TRANSLATE DX=$sp_x DY=$sp_y
  SCALE FACTOR=$sp_s
  IF $INDEX % 3 == 0 THEN
    IF $mood == 2 THEN
      COLOR NAME=WHITE
    ELSE
      COLOR NAME=BLACK
    ENDIF
  ELSE
    IF $mood == 2 THEN
      COLOR NAME=BLACK
    ELSE
      COLOR NAME=WHITE
    ENDIF
  ENDIF
  DRAW NAME="halo20" X=-10 Y=-10
ENDREPEAT
RESET_TRANSFORMS

# ---- reflection: glint block over a halo clearing, zigzagging ------
# Each step first stamps the halo block in the paper's ink -- a fuzzy
# dithered clearing in the waves -- then the glint block in figure
# ink on top of it. Short at noon, long when the sun is low.
VAR $refl_step = $g_s * 8
VAR $refl_len = $sea_h / 4 + $noon_d * $sea_h / 20
IF $refl_len > $sea_h THEN
  LET $refl_len = $sea_h
ENDIF
VAR $refl_count = $refl_len / $refl_step
VAR $zig = 0
VAR $rx = 0
VAR $ry = 0
REPEAT COUNT=$refl_count
  LET $seed = $seed * 2053 + 13849
  LET $seed = $seed % 32749
  LET $rnd = $seed / 8
  LET $zig = $INDEX % 2 * $sun_r / 2 - $sun_r / 4
  LET $rx = $sun_x + $zig + $rnd % 7 - 3
  LET $ry = $INDEX * $refl_step + $horizon + $hline + 2

  RESET_TRANSFORMS
  TRANSLATE DX=$rx DY=$ry
  SCALE FACTOR=$g_s
  IF $mood == 2 THEN
    COLOR NAME=BLACK
  ELSE
    COLOR NAME=WHITE
  ENDIF
  DRAW NAME="halo20" X=-10 Y=-7
  IF $mood == 2 THEN
    COLOR NAME=WHITE
  ELSE
    COLOR NAME=BLACK
  ENDIF
  DRAW NAME="glint20" X=-10 Y=-3
ENDREPEAT
RESET_TRANSFORMS

# ---- sea stacks, when the tide is out ------------------------------
# One block, five stamps: four in white offset one unit each way make
# the contour, the fifth in black is the rock. Each stack leans its
# own few degrees and prints at its own scale; the foam block breaks
# across its waterline in the ink of the waves.
IF $exposure > 0 THEN
  VAR $rock_n = $exposure + 1
  VAR $rk_sp = $WIDTH / $rock_n
  IF $rk_sp < 1 THEN
    LET $rk_sp = 1
  ENDIF
  VAR $rock_x = 0
  VAR $rock_y = 0
  VAR $rock_s = 0
  VAR $rock_rot = 0
  VAR $rock_base = $HEIGHT - $sea_h / 6
  VAR $rk_span = $sea_h / 3 + 1
  VAR $rk_smax = $sea_h / 60 + 1
  REPEAT COUNT=$rock_n
    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_x = $INDEX * $rk_sp + $rnd % $rk_sp

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_y = $rock_base - $rnd % $rk_span

    LET $seed = $seed * 2053 + 13849
    LET $seed = $seed % 32749
    LET $rnd = $seed / 8
    LET $rock_s = $rnd % $rk_smax + $exposure + 1
    LET $rock_rot = $rnd % 13 + 353
    LET $rock_rot = $rock_rot % 360

    RESET_TRANSFORMS
    TRANSLATE DX=$rock_x DY=$rock_y
    SCALE FACTOR=$rock_s
    ROTATE DEGREES=$rock_rot
    COLOR NAME=WHITE
    DRAW NAME="rock20" X=-11 Y=-21
    DRAW NAME="rock20" X=-9 Y=-21
    DRAW NAME="rock20" X=-10 Y=-22
    DRAW NAME="rock20" X=-10 Y=-20
    COLOR NAME=BLACK
    DRAW NAME="rock20" X=-10 Y=-21
    IF $mood == 2 THEN
      COLOR NAME=WHITE
    ELSE
      COLOR NAME=BLACK
    ENDIF
    DRAW NAME="foam20" X=-16 Y=-6
    DRAW NAME="foam20" X=0 Y=-4
  ENDREPEAT
  RESET_TRANSFORMS
ENDIF
