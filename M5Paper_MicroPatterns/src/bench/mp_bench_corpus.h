// GENERATED FILE -- do not edit by hand.
// Regenerate with: python3 tools/device_bench/gen_corpus.py
// Source: tools/host_harness/corpus/*.mp (shared with the host harness)
#ifndef MP_BENCH_CORPUS_H
#define MP_BENCH_CORPUS_H

#if MP_BENCH

// artdeco_default.mp  (1236 bytes, sha256:e478e3d8a74b4fe6)
static const char kScript_artdeco_default[] = R"MPBCORPUS(
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
)MPBCORPUS";

// city.mp  (7447 bytes, sha256:01e7d82584525a1f)
static const char kScript_city[] = R"MPBCORPUS(# City Map Generator - MicroPatterns DSL
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
ENDREPEAT)MPBCORPUS";

// emulator_welcome.mp  (1508 bytes, sha256:1cda0350f17746bd)
static const char kScript_emulator_welcome[] = R"MPBCORPUS(
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
                    
        )MPBCORPUS";

struct MPBenchScript {
    const char* name;
    const char* text;
    unsigned int bytes;
    const char* sha256_16;  // first 16 hex chars of sha256 of the source bytes
};

static const MPBenchScript kMPBenchCorpus[] = {
    {"artdeco_default", kScript_artdeco_default, 1236, "e478e3d8a74b4fe6"},
    {"city", kScript_city, 7447, "01e7d82584525a1f"},
    {"emulator_welcome", kScript_emulator_welcome, 1508, "1cda0350f17746bd"},
};
static const int kMPBenchCorpusCount = 3;

#endif // MP_BENCH
#endif // MP_BENCH_CORPUS_H
