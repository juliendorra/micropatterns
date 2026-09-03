// GENERATED FILE -- do not edit by hand.
// Regenerate with: python3 tools/device_bench/gen_ops_corpus.py
#ifndef WATCHY_BENCH_CORPUS_H
#define WATCHY_BENCH_CORPUS_H

#if MP_BENCH

struct MPBenchScript { const char* kind; const char* name; const char* src; };

// tools/device_bench/ops/op_circle_outline.mp  (333 bytes, sha256:d4ca47ad07105588)
static const char kMPB_op_circle_outline[] = R"MPB(# CONTROL GROUP. One transformed centre, one radius, then the integer midpoint
# circle algorithm. Nothing per-pixel is transformed at all.
VAR $i = 0
VAR $c = 0
COLOR NAME=BLACK
LET $c = $WIDTH / 2
REPEAT COUNT=40
  LET $i = $INDEX
  RESET_TRANSFORMS
  TRANSLATE DX=$c DY=$c
  ROTATE DEGREES=23
  CIRCLE X=0 Y=0 RADIUS=$i
ENDREPEAT
)MPB";

// tools/device_bench/ops/op_draw_asset.mp  (412 bytes, sha256:fc290ad559ead556)
static const char kMPB_op_draw_asset[] = R"MPB(# DRAW of a scaled-up asset. A separate path from FILL_*: it walks the asset's
# own pixels rather than a screen-space span, so the per-pixel cost is a
# different loop over the same transform.
DEFINE PATTERN NAME="a8" WIDTH=8 HEIGHT=8 DATA="1100110011001100001100110011001111001100110011000011001100110011"
VAR $s = 0
COLOR NAME=BLACK
LET $s = $WIDTH / 8
RESET_TRANSFORMS
SCALE FACTOR=$s
DRAW NAME="a8" X=0 Y=0
)MPB";

// tools/device_bench/ops/op_fill_circle.mp  (249 bytes, sha256:45036ac06ca87dac)
static const char kMPB_op_fill_circle[] = R"MPB(# Solid disk inscribed in the canvas. Adds the exact disk test
# dx*dx + dy*dy <= r*r and the narrowSpan pass on top of the plain fill.
VAR $r = 0
COLOR NAME=BLACK
FILL NAME=SOLID
LET $r = $WIDTH / 2
RESET_TRANSFORMS
FILL_CIRCLE X=$r Y=$r RADIUS=$r
)MPB";

// tools/device_bench/ops/op_fill_circle_pattern_rot.mp  (428 bytes, sha256:28b0b1b7af62a804)
static const char kMPB_op_fill_circle_pattern_rot[] = R"MPB(# Rotated patterned disk: the disk test plus the un-hoistable per-pixel
# transform. Also the shape whose bounds undershot before 2026-09-03, so a
# regression there shows up here as a drop in BOTH time and ink.
DEFINE PATTERN NAME="p4" WIDTH=4 HEIGHT=4 DATA="1000010000100001"
VAR $r = 0
COLOR NAME=BLACK
FILL NAME="p4"
LET $r = $WIDTH / 2
RESET_TRANSFORMS
TRANSLATE DX=$r DY=$r
ROTATE DEGREES=23
FILL_CIRCLE X=0 Y=0 RADIUS=$r
)MPB";

// tools/device_bench/ops/op_fill_pixel.mp  (632 bytes, sha256:1d56bf372f750b76)
static const char kMPB_op_fill_pixel[] = R"MPB(# FILL_PIXEL one at a time: the most transform-heavy path per painted pixel.
# Every pixel pays its own display-list item, its own inverse transform and its
# own pattern lookup, with no span to amortise over. Kept well inside the canvas
# so the rotation cannot push items off-screen and quietly shrink the workload.
DEFINE PATTERN NAME="p4" WIDTH=4 HEIGHT=4 DATA="1000010000100001"
VAR $i = 0
VAR $c = 0
VAR $k = 0
COLOR NAME=BLACK
FILL NAME="p4"
LET $c = $WIDTH / 2
LET $k = 0 - 60
RESET_TRANSFORMS
TRANSLATE DX=$c DY=$c
ROTATE DEGREES=23
REPEAT COUNT=60
  LET $i = $INDEX
  FILL_PIXEL X=$k Y=$i
  FILL_PIXEL X=$i Y=$k
ENDREPEAT
)MPB";

// tools/device_bench/ops/op_fill_rect_pattern.mp  (301 bytes, sha256:47ffb1ee848adb35)
static const char kMPB_op_fill_rect_pattern[] = R"MPB(# Patterned FILL_RECT, unrotated: im1 == 0, so the rasteriser hoists the pattern
# ROW out of the scanline and transforms only x per pixel.
DEFINE PATTERN NAME="p4" WIDTH=4 HEIGHT=4 DATA="1000010000100001"
COLOR NAME=BLACK
FILL NAME="p4"
RESET_TRANSFORMS
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
)MPB";

// tools/device_bench/ops/op_fill_rect_pattern_rot.mp  (932 bytes, sha256:5fc8826c796c9fef)
static const char kMPB_op_fill_rect_pattern_rot[] = R"MPB(# The same patterned fill rotated, so im1 != 0 and the row hoist is impossible:
# both inverse coordinates are computed per pixel, then unscaled. This is the
# transform-bound worst case and the one a fixed-point port targets.
#
# Sized 2x the canvas and centred so the rotated rect still covers every pixel.
# At 23 degrees a canvas-sized rect leaves the corners bare, and an earlier
# version of this probe translated to the corner and rendered NOTHING AT ALL --
# which is why "rendered items" is checked, not just "it parsed".
DEFINE PATTERN NAME="p4" WIDTH=4 HEIGHT=4 DATA="1000010000100001"
VAR $cx = 0
VAR $cy = 0
VAR $w2 = 0
VAR $h2 = 0
VAR $nx = 0
VAR $ny = 0
COLOR NAME=BLACK
FILL NAME="p4"
LET $cx = $WIDTH / 2
LET $cy = $HEIGHT / 2
LET $w2 = $WIDTH * 2
LET $h2 = $HEIGHT * 2
LET $nx = 0 - $WIDTH
LET $ny = 0 - $HEIGHT
RESET_TRANSFORMS
TRANSLATE DX=$cx DY=$cy
ROTATE DEGREES=23
FILL_RECT X=$nx Y=$ny WIDTH=$w2 HEIGHT=$h2
)MPB";

// tools/device_bench/ops/op_fill_rect_solid.mp  (475 bytes, sha256:16a97ebfb54746a9)
static const char kMPB_op_fill_rect_solid[] = R"MPB(# Solid FILL_RECT covering the canvas once. The cheapest per-pixel path there
# is: no pattern lookup, no per-pixel divide, full-width spans.
#
# Coverage is ONE canvas per render, not N. Repeating a full-canvas fill is
# pointless -- the occlusion buffer correctly culls every item after the first,
# so 24 copies measured exactly the same work as 1. Statistics come from reps.
COLOR NAME=BLACK
FILL NAME=SOLID
RESET_TRANSFORMS
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
)MPB";

// tools/device_bench/ops/op_line.mp  (281 bytes, sha256:b2044696bbd9059c)
static const char kMPB_op_line[] = R"MPB(# CONTROL GROUP. Bresenham already runs on integers; a fixed-point transform
# port must not move this number. Only the two endpoints are transformed.
VAR $i = 0
COLOR NAME=BLACK
REPEAT COUNT=40
  LET $i = $INDEX
  RESET_TRANSFORMS
  LINE X1=0 Y1=$i X2=$WIDTH Y2=$HEIGHT
ENDREPEAT
)MPB";

// tools/device_bench/ops/op_rect_outline.mp  (277 bytes, sha256:a8ed6e912fb648e0)
static const char kMPB_op_rect_outline[] = R"MPB(# CONTROL GROUP. Four transformed corners, then four integer Bresenham runs.
VAR $i = 0
VAR $c = 0
COLOR NAME=BLACK
LET $c = $WIDTH / 4
REPEAT COUNT=40
  LET $i = $INDEX
  RESET_TRANSFORMS
  TRANSLATE DX=$c DY=$c
  ROTATE DEGREES=23
  RECT X=0 Y=0 WIDTH=$i HEIGHT=$i
ENDREPEAT
)MPB";

// tools/host_harness/corpus/artdeco_default.mp  (1236 bytes, sha256:e478e3d8a74b4fe6)
static const char kMPB_artdeco_default[] = R"MPB(
DEFINE PATTERN NAME="artdeco" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000010000001000000000001000000001000000000100000000001000000010000000000001000001000000000000001000000000000010000000000000001000010000000000000100000010000000000010000000010000000001000000000010000000100000000000010000010000000000000000000000000000000000000000000000"

VAR $center_x
VAR $center_y
VAR $secondplus
VAR $rotation
VAR $size

# fill background
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT WIDTH=$WIDTH HEIGHT=$HEIGHT X=0 Y=0

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $secondplus = 3 + $SECOND * $counter % 15
LET $rotation = 360 * 89 / $secondplus
ROTATE DEGREES=$rotation

LET $size = $width / 40

FILL NAME="artdeco"
COLOR NAME=BLACK

REPEAT COUNT=$secondplus

ROTATE DEGREES=$rotation

VAR $radius = $INDEX * 10 % 50
VAR $Xposition= 0
VAR $Yposition= $INDEX

FILL_CIRCLE RADIUS=$INDEX X=$Xposition Y=$Yposition

IF $INDEX % 2 == 0 THEN
COLOR NAME=WHITE
SCALE FACTOR=$size
ELSE
COLOR NAME=BLACK
SCALE FACTOR=$size
ENDIF

DRAW name="artdeco" x=$Xposition y=$Yposition

ENDREPEAT
)MPB";

// tools/host_harness/corpus/bounds.mp  (2500 bytes, sha256:bf5bdbb3a8375fca)
static const char kMPB_bounds[] = R"MPB(# Regression gate for two bounds bugs found 2026-09-03. Both were invisible to
# every other corpus script, and one of them rendered NOTHING AT ALL -- which is
# exactly the kind of failure a golden can only catch if some script provokes it.
#
# 1. An axis-aligned LINE has an AABB of zero thickness. floor/ceil collapsed it
#    to minY == maxY, the "clipped away to nothing" test read that as off-screen,
#    and the item was culled. EVERY horizontal and vertical LINE drew nothing.
#    Measured: ROTATE 0 / 90 / 180 gave 0 px, ROTATE 1 / 89 / 91 gave all 401.
#    prims.mp draws lines only inside a ROTATE $i loop, so it never hit 0/90/180.
#
# 2. FILL_CIRCLE bounded itself with the AABB of an INSCRIBED OCTAGON -- eight
#    sampled points, four of them via a 0.7071 diagonal offset -- which
#    undershoots a rotated disk by r*(1 - cos 22.5deg) = 7.6% of the radius.
#    Measured on radius 200: 400 px wide at ROTATE 0, clipped to 372 px at
#    ROTATE 22, losing 4.4% of the area and drawing eight flat sides.
#
# Deliberately static: it must render the same at every seed, so a failure here
# is unambiguously a renderer change and never a clock or counter difference.

COLOR NAME=BLACK

# --- 1. axis-aligned lines, untransformed --------------------------------
RESET_TRANSFORMS
LINE X1=20 Y1=40 X2=320 Y2=40
LINE X1=40 Y1=60 X2=40 Y2=360

# --- 1b. axis-aligned lines reached THROUGH a rotation -------------------
# The transform, not the operands, is what makes these axis-aligned on screen.
RESET_TRANSFORMS
TRANSLATE DX=200 DY=460
ROTATE DEGREES=90
LINE X1=-150 Y1=0 X2=150 Y2=0

RESET_TRANSFORMS
TRANSLATE DX=420 DY=180
ROTATE DEGREES=180
LINE X1=-120 Y1=0 X2=120 Y2=0

RESET_TRANSFORMS
TRANSLATE DX=420 DY=300
ROTATE DEGREES=270
LINE X1=-100 Y1=0 X2=100 Y2=0

# A zero-length LINE is degenerate in BOTH axes at once.
RESET_TRANSFORMS
LINE X1=500 Y1=500 X2=500 Y2=500

# --- 2. rotated filled disks, at and around the worst-case angle ---------
# 22 degrees is where the octagon AABB undershot hardest.
RESET_TRANSFORMS
TRANSLATE DX=640 DY=180
ROTATE DEGREES=22
FILL_CIRCLE X=0 Y=0 RADIUS=120

RESET_TRANSFORMS
TRANSLATE DX=640 DY=400
ROTATE DEGREES=67
SCALE FACTOR=2
FILL_CIRCLE X=0 Y=0 RADIUS=55

# The outline path computed its extent correctly all along; drawn concentric
# with a filled disk so a future divergence between the two is visible.
RESET_TRANSFORMS
TRANSLATE DX=870 DY=270
ROTATE DEGREES=22
FILL_CIRCLE X=0 Y=0 RADIUS=60
COLOR NAME=WHITE
CIRCLE X=0 Y=0 RADIUS=60
)MPB";

// tools/host_harness/corpus/city.mp  (7447 bytes, sha256:01e7d82584525a1f)
static const char kMPB_city[] = R"MPB(# City Map Generator - MicroPatterns DSL
# Creates a procedural city layout using 8 different 20x20 tile patterns
# Adapts to any screen size using $WIDTH and $HEIGHT

# Define 8 city tile patterns (20x20 each)

# Pattern 1: Empty lot/grass
DEFINE PATTERN NAME="empty" WIDTH=20 HEIGHT=20 DATA="00000000000000000000000100000001000000000010000100000000000100010000000000000001000100000000000000010001000000000000000100010000000000000001000100000000000000010001000000000000000100010000000000000001000100000000000000010001000000000000000100010000000000000001000100000000000000010001000000000000000100010000000000000001000100000000000000010001000000000000000000000000000000000000"

# Pattern 2: Building block (solid)
DEFINE PATTERN NAME="building" WIDTH=20 HEIGHT=20 DATA="11111111111111111111100100100100100100110010010010010010011001001001001001001100100100100100100110010010010010010011001001001001001001100100100100100100110010010010010010011001001001001001001100100100100100100110010010010010010011001001001001001001100100100100100100110010010010010010011001001001001001001100100100100100100111111111111111111111"

# Pattern 3: Horizontal road
DEFINE PATTERN NAME="road_h" WIDTH=20 HEIGHT=20 DATA="00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001111111111111111111100000000000000000000011111111111111111110000000000000000000011111111111111111111000000000000000000001111111111111111111100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

# Pattern 4: Vertical road
DEFINE PATTERN NAME="road_v" WIDTH=20 HEIGHT=20 DATA="00000111100000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111"

# Pattern 5: Intersection (cross roads)
DEFINE PATTERN NAME="intersection" WIDTH=20 HEIGHT=20 DATA="00000111100000111100000011110000001111000000111100000011110000001111000000111100000011111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111000000111100000011110000001111"

# Pattern 6: Park/green space
DEFINE PATTERN NAME="park" WIDTH=20 HEIGHT=20 DATA="00010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001000100010001"

# Pattern 7: Dense building area
DEFINE PATTERN NAME="dense" WIDTH=20 HEIGHT=20 DATA="11111111110111111111111111111011111111111111111101111111111111111110111111111111111111011111111111111111101111111111111111110111111111111111111011111111111111111101111111111111111110111111111111111111011111111111111111101111111111111111110111111111111111111011111111111111111101111111111111111110111111111111111111011111111"

# Pattern 8: Commercial strip
DEFINE PATTERN NAME="commercial" WIDTH=20 HEIGHT=20 DATA="11110000111100001111000011110000111100000000000000000000000000000000000011110000111100001111000011110000111100000000000000000000000000000000000011110000111100001111000011110000111100000000000000000000000000000000000011110000111100001111000011110000111100000000000000000000000000000000000011110000111100001111000011110000"

VAR $scaling=4

# Calculate grid dimensions (how many 20x20 scaled up tiles fit)
VAR $grid_width = $WIDTH / 20 * $scaling
VAR $grid_height = $HEIGHT / 20 *$scaling

# Ensure minimum grid size
IF $grid_width < 1 THEN
    LET $grid_width = 1
ENDIF
IF $grid_height < 1 THEN
    LET $grid_height = 1
ENDIF

# Variables for city generation with more randomness sources
VAR $seed = $HOUR * 60 + $MINUTE + $COUNTER
VAR $time_factor = $SECOND * 3 + $MINUTE / 10
VAR $counter_mod = $COUNTER % 17
VAR $x
VAR $y
VAR $tile_type
VAR $pattern_choice

# Fill background
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

# Generate city grid
REPEAT COUNT=$grid_height

    LET $y = $INDEX
    
    REPEAT COUNT=$grid_width
        LET $x = $INDEX
        
        # Generate pseudo-random tile type with multiple randomness sources
        VAR $x_factor = $x * 13
        VAR $y_factor = $y * 19
        VAR $xy_cross = $x * $y * 7
        VAR $time_influence = $time_factor * 23
        VAR $counter_influence = $counter_mod * 31
        VAR $random_sum = $x_factor + $y_factor + $xy_cross + $seed + $time_influence + $counter_influence
        LET $tile_type = $random_sum % 100
                
        # Choose pattern based on tile_type value
        IF $tile_type < 5 THEN
            # empty lot
            LET $pattern_choice = 1
        ENDIF
        
        IF $tile_type >= 5 THEN
            IF $tile_type < 25 THEN
                # building
                LET $pattern_choice = 2
            ENDIF
        ENDIF
        
        IF $tile_type >= 25 THEN
            IF $tile_type < 35 THEN
                # horizontal road
                LET $pattern_choice = 3
            ENDIF
        ENDIF
        
        IF $tile_type >= 35 THEN
            IF $tile_type < 45 THEN
                # vertical road
                LET $pattern_choice = 4
            ENDIF
        ENDIF
        
        IF $tile_type >= 45 THEN
            IF $tile_type < 50 THEN
                # intersection
                LET $pattern_choice = 5
            ENDIF
        ENDIF
        
        IF $tile_type >= 50 THEN
            IF $tile_type < 60 THEN
                # park
                LET $pattern_choice = 6
            ENDIF
        ENDIF
        
        IF $tile_type >= 60 THEN
            IF $tile_type < 80 THEN
                # dense building
                LET $pattern_choice = 7
            ENDIF
        ENDIF
        
        IF $tile_type >= 80 THEN
            # commercial strip
            LET $pattern_choice = 8
        ENDIF
        
        # Calculate grid position (20x20 spacing)
        VAR $pixel_x = $x * 20
        VAR $pixel_y = $y * 20
        
        # Apply 4x scale 
        COLOR NAME=BLACK
        SCALE FACTOR=$scaling

        IF $pattern_choice == 1 THEN
            DRAW NAME="empty" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 2 THEN
            DRAW NAME="building" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 3 THEN
            DRAW NAME="road_h" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 4 THEN
            DRAW NAME="road_v" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 5 THEN
            DRAW NAME="intersection" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 6 THEN
            DRAW NAME="park" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 7 THEN
            DRAW NAME="dense" X=$pixel_x Y=$pixel_y
        ENDIF
        
        IF $pattern_choice == 8 THEN
            DRAW NAME="commercial" X=$pixel_x Y=$pixel_y
        ENDIF
        
        # Reset scale after drawing
        RESET_TRANSFORMS
        
    ENDREPEAT
ENDREPEAT)MPB";

// tools/host_harness/corpus/emulator_welcome.mp  (1508 bytes, sha256:1cda0350f17746bd)
static const char kMPB_emulator_welcome[] = R"MPB(
# Welcome to MicroPatterns!
# Display is 200x200

# Define patterns using DEFINE PATTERN
DEFINE PATTERN NAME="checker" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="smile" WIDTH=8 HEIGHT=8 DATA="0111111010000001101001011000000110100101100110011000000101111110"

# Declare variables used later (require $ prefix)
VAR $center_x
VAR $center_y
VAR $bar_height
VAR $secondplusone
VAR $rotation
VAR $size

COLOR NAME=BLACK
# Use defined pattern name with FILL
FILL NAME="checker"
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

COLOR NAME=WHITE
# Reset to solid fill
FILL NAME=SOLID
# Use $ prefix for LET assignment target
LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

# Use declared variables (references already use $)
TRANSLATE DX=$center_x DY=$center_y

# Expressions are only supported in assignements
LET $secondplusone = 1 + $SECOND
LET $rotation = 350 * 59 / $secondplusone
ROTATE DEGREES=$rotation

LET $size = 1 + $COUNTER % 20
SCALE FACTOR=$size

# Center the 8x8 pattern using DRAW
DRAW NAME="smile" X=-4 Y=-4

RESET_TRANSFORMS
COLOR NAME=BLACK
LET $bar_height = $COUNTER % 50 + 10
# Use declared variable
FILL_RECT X=10 Y=180 WIDTH=30 HEIGHT=$bar_height 

# Draw a line using FILL_PIXEL
# It will only draw where the background 'checker' pattern is 1
COLOR NAME=WHITE
# Set fill to SOLID so pattern check uses COLOR
FILL NAME=SOLID 
VAR $diag_pos
REPEAT COUNT=100
    LET $diag_pos = $INDEX + 50
    FILL_PIXEL X=$diag_pos Y=$diag_pos
ENDREPEAT
                    
        )MPB";

// tools/host_harness/corpus/i32.mp  (682 bytes, sha256:a2c4173567855002)
static const char kMPB_i32[] = R"MPB(# Integer-width probe. 100000*100000 = 10,000,000,000, which does not fit in
# int32. A 32-bit engine wraps to 1,410,065,408; a double-based engine keeps the
# exact value. Taking % 97 turns that into a visible x position: 76 vs 49.
VAR $a = 100000
LET $a = $a * 100000
LET $a = $a % 97
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT X=$a Y=10 WIDTH=4 HEIGHT=4

# Second case: negative wrap. 2000000000 + 2000000000 overflows to a NEGATIVE
# int32 on the device. A double-based engine gets 4e9 and stays positive, so the
# two disagree about whether the rect is drawn at all.
VAR $b = 2000000000
LET $b = $b + $b
LET $b = $b % 211
LET $b = $b + 300
FILL_RECT X=$b Y=30 WIDTH=4 HEIGHT=4
)MPB";

// tools/host_harness/corpus/nest.mp  (532 bytes, sha256:f12ca2e948a844c8)
static const char kMPB_nest[] = R"MPB(DEFINE PATTERN NAME="chk" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="sol" WIDTH=2 HEIGHT=2 DATA="1111"
VAR $a = 0
VAR $b = 0
REPEAT COUNT=8
  LET $a = $INDEX
  REPEAT COUNT=6
    LET $b = $INDEX * 3 + $a % 4 - 1
    IF $b > 4 THEN
      FILL NAME="chk"
      COLOR NAME=BLACK
      FILL_RECT X=$b Y=$a WIDTH=25 HEIGHT=15
    ELSE
      FILL NAME=SOLID
      COLOR NAME=WHITE
      DRAW NAME="sol" X=$b Y=$a
    ENDIF
    TRANSLATE DX=12 DY=7
    DRAW NAME="chk" X=$a Y=$b
  ENDREPEAT
  RESET_TRANSFORMS
ENDREPEAT
)MPB";

// tools/host_harness/corpus/prims.mp  (559 bytes, sha256:7cb4603ea7e1a404)
static const char kMPB_prims[] = R"MPB(# exercises the primitives the committed corpus does not: LINE, RECT, CIRCLE, PIXEL
VAR $i = 0
COLOR NAME=BLACK
REPEAT COUNT=12
  LET $i = $INDEX
  RESET_TRANSFORMS
  TRANSLATE DX=$i DY=$i
  ROTATE DEGREES=$i
  SCALE FACTOR=2
  LINE X1=$i Y1=10 X2=200 Y2=$i
  RECT X=$i Y=40 WIDTH=60 HEIGHT=30
  CIRCLE X=120 Y=90 RADIUS=25
  PIXEL X=$i Y=5
  FILL_PIXEL X=$i Y=8
  RESET_TRANSFORMS
  SCALE FACTOR=3
  COLOR NAME=WHITE
  RECT X=10 Y=$i WIDTH=20 HEIGHT=20
  COLOR NAME=BLACK
  FILL_CIRCLE X=$i Y=140 RADIUS=9
  FILL_RECT X=200 Y=$i WIDTH=40 HEIGHT=12
ENDREPEAT
)MPB";

static const MPBenchScript kMPBenchScripts[] = {
    { "op", "op_circle_outline", kMPB_op_circle_outline },
    { "op", "op_draw_asset", kMPB_op_draw_asset },
    { "op", "op_fill_circle", kMPB_op_fill_circle },
    { "op", "op_fill_circle_pattern_rot", kMPB_op_fill_circle_pattern_rot },
    { "op", "op_fill_pixel", kMPB_op_fill_pixel },
    { "op", "op_fill_rect_pattern", kMPB_op_fill_rect_pattern },
    { "op", "op_fill_rect_pattern_rot", kMPB_op_fill_rect_pattern_rot },
    { "op", "op_fill_rect_solid", kMPB_op_fill_rect_solid },
    { "op", "op_line", kMPB_op_line },
    { "op", "op_rect_outline", kMPB_op_rect_outline },
    { "script", "artdeco_default", kMPB_artdeco_default },
    { "script", "bounds", kMPB_bounds },
    { "script", "city", kMPB_city },
    { "script", "emulator_welcome", kMPB_emulator_welcome },
    { "script", "i32", kMPB_i32 },
    { "script", "nest", kMPB_nest },
    { "script", "prims", kMPB_prims },
};
static const int MPBENCH_SCRIPT_COUNT = 17;

#endif // MP_BENCH
#endif // WATCHY_BENCH_CORPUS_H
