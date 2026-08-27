#ifndef EMBEDDED_SCRIPTS_H
#define EMBEDDED_SCRIPTS_H

// A few of the real scripts, lifted from the device backup taken before the
// SPIFFS restore (tools/device/backups/2026-08-27-143524). Embedded in flash
// so this first Watchy build needs no filesystem, no WiFi and no server --
// it just boots and draws. Stored in PROGMEM-friendly const char arrays.

#include <Arduino.h>

struct EmbeddedScript { const char* id; const char* name; const char* content; };

static const char SCRIPT_S4[] = R"MPS(
DEFINE PATTERN NAME="pipes" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111000000011111111111110000001111111111111000000011111100000000000000111000000000000000001110001111111111100011111111111111111100111111111111111111000111110000000001110000000000000000011100000000000001111111000000000000011111110000000000000111000000000000"


DEFINE PATTERN NAME="pipes2" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011100111111111111111111101111111111111111111001111111111111100110000000000000000001100000000000000001111001111111111111111110011111111111111111001111111111111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"


VAR $center_x
VAR $center_y
VAR $secondplus
VAR $rotation
VAR $size

# fill background
SCALE FACTOR= 2
COLOR NAME=BLACK
FILL NAME="pipes"
FILL_RECT WIDTH=$WIDTH HEIGHT=$HEIGHT X=0 Y=0

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $secondplus = 1 + $SECOND
LET $rotation = 360 * 30 / $secondplus
ROTATE DEGREES=$rotation

VAR $halfsize = $size/2

COLOR NAME=BLACK

REPEAT COUNT=$secondplus

 VAR $indexmod = $index % 3
  VAR $countermod = $counter % 5
 LET $size = 5 +  $countermod * $indexmod

 ROTATE DEGREES=$rotation

 VAR $Xposition= $INDEX
 VAR $Yposition= $INDEX
 SCALE FACTOR=$size
 
 IF $INDEX % 2 == 0 THEN
   FILL NAME="pipes"

 ELSE
  FILL NAME="pipes2"
 
 ENDIF
 
  VAR $rectWidth= $WiDTH/50
  VAR $rectHeight= $HEIGHT/50
 
 FILL_RECT width=$rectWidth height=$rectWidth X=$Xposition Y=$Yposition
  
ENDREPEAT


)MPS";

static const char SCRIPT_S5[] = R"MPS(DEFINE PATTERN NAME="zigzag" WIDTH=20 HEIGHT=20 DATA="0000000000011000000000000011000011000000100000011100011000001110000011100011000001110000011000000000011110001111000000000011100001111000000000001110001110000100000011110000110011000000001110001110000001100011100101110000001000011100001110000000000011100000100001000000011100000110011000000011100000110011000000001100000100011100000001100000000011100000000100000000011100000000000000000001100000000000"


VAR $center_x
VAR $center_y
VAR $secondplus = 30 + $SECOND
VAR $secondplusone = 1 + $SECOND
VAR $rotation
VAR $size
VAR $counterplusmod = 12 + $counter % $secondplusone

# fill background
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT WIDTH=$WIDTH HEIGHT=$HEIGHT X=0 Y=0

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $rotation = 720 * 89 / $secondplus
ROTATE DEGREES=$rotation

LET $size = 10 + $COUNTER % 10
VAR $halfsize = $size/2

FILL NAME="zigzag"
COLOR NAME=BLACK

REPEAT COUNT=$counterplusmod

 ROTATE DEGREES=$rotation
 VAR $shift = $MINUTE / $secondplusone
 TRANSLATE dx=$shift dy=$shift

 VAR $radius = $INDEX * 10 % 50
 VAR $Xposition= $INDEX
 VAR $Yposition= $INDEX

 IF $INDEX % 2 == 0 THEN
  COLOR NAME=WHITE
  SCALE FACTOR=$size
 ELSE
  COLOR NAME=BLACK
  SCALE FACTOR=$halfsize
 ENDIF
 
 FILL_RECT WIDTH=$INDEX HEIGHT=$INDEX X=$Xposition Y=$Yposition
 
 IF $INDEX % 3 == 0 THEN
  COLOR NAME=WHITE
  SCALE FACTOR=$size
 ELSE
  COLOR NAME=BLACK
  SCALE FACTOR=$halfsize
 ENDIF
 
 ROTATE DEGREES=$rotation
 FILL_RECT WIDTH=$INDEX HEIGHT=$INDEX X=$Xposition Y=$Yposition

 
 DRAW name="zigzag" x=$Xposition y=$Yposition
 
ENDREPEAT


)MPS";

static const char SCRIPT_S3[] = R"MPS(DEFINE PATTERN NAME="eye1" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000100100100010001000001001001000100110000011011011111001001000010011100011110110000111100000001111000111000011111000010001000001110011000010010000111100011000100100001111000110001001100011110011100010001100011111110001100001100011111100010000000110001110001000000000111000000100000000000011111110000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
DEFINE PATTERN NAME="eye2" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000000000000000000000000000000000000000000001111110000000000001100000010000000000110000000010000000001000011100010000000110001111100010000001000111111100100000010001110011001000000100011100110010000001000111111100100000010001111110010000000010001111000100000000110000000011000000000110000001100000000000011111110000000000000000000000000000000000000000000000"
DEFINE PATTERN NAME="eye3" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000010000000000000001000101001001001000010001010010010010000110010110100010100000100110101001001000001110111111111110000010000000000111000001100001111110100000110000111110101000000100001111101010000000100011111110100000001000111111110000000001001111110100000000011000011011000000000011000000100000000000001111110000000000000000000000000000000000000000000000"

# Set up background
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT

# Calculate how many eyes can fit horizontally and vertically
VAR $eye_size = 20
VAR $grid_cols = $WIDTH / $eye_size
VAR $grid_rows = $HEIGHT / $eye_size

# Set up variables for random placement
VAR $seed_factor1 = 17
VAR $seed_factor2 = 31
VAR $seed_step1 = $SECOND * $seed_factor1
VAR $seed = $seed_step1 + $COUNTER * $seed_factor2

# Number of eyes to place
VAR $num_eyes_raw = $grid_cols * $grid_rows
VAR $num_eyes = $num_eyes_raw / 3
VAR $max_eyes = 50
IF $num_eyes > $max_eyes THEN
  LET $num_eyes = $max_eyes
ENDIF

# Set the drawing color
COLOR NAME=BLACK

# Constants for our pseudo-random number generator
VAR $multiplier = 1103515245
VAR $increment = 12345
VAR $modulus = 32768

# Variables for temporary calculations
VAR $seed_temp = 0
VAR $seed_temp2 = 0
VAR $offset_mod = 0
VAR $grid_x = 0
VAR $grid_y = 0
VAR $offset_x = 0
VAR $offset_y = 0
VAR $x = 0
VAR $y = 0
VAR $eye_type_mod = 0
VAR $eye_type = 0
VAR $position_used = 0
VAR $max_x = 0
VAR $max_y = 0
VAR $rotate_chance = 0
VAR $rotation = 0

# Place eyes randomly
REPEAT COUNT=$num_eyes
  # Generate pseudo-random values for this eye
  LET $seed_temp = $seed * $multiplier
  LET $seed_temp2 = $seed_temp + $increment
  LET $seed = $seed_temp2 % $modulus
  
  # Calculate grid position
  LET $grid_x = $seed % $grid_cols
  
  LET $seed_temp = $seed * $multiplier
  LET $seed_temp2 = $seed_temp + $increment
  LET $seed = $seed_temp2 % $modulus
  
  LET $grid_y = $seed % $grid_rows
  
  # Add small random variation to position
  LET $seed_temp = $seed * $multiplier
  LET $seed_temp2 = $seed_temp + $increment
  LET $seed = $seed_temp2 % $modulus
  
  LET $offset_mod = $seed % 5
  LET $offset_x = $offset_mod - 2
  
  LET $seed_temp = $seed * $multiplier
  LET $seed_temp2 = $seed_temp + $increment
  LET $seed = $seed_temp2 % $modulus
  
  LET $offset_mod = $seed % 5
  LET $offset_y = $offset_mod - 2
  
  # Calculate final position
  LET $x = $grid_x * $eye_size + $offset_x
  LET $y = $grid_y * $eye_size + $offset_y
  
  # Choose which eye to draw (1, 2, or 3)
  LET $seed_temp = $seed * $multiplier
  LET $seed_temp2 = $seed_temp + $increment
  LET $seed = $seed_temp2 % $modulus
  
  LET $eye_type_mod = $seed % 3
  LET $eye_type = $eye_type_mod + 1
  
  # Check if position is already used (simple collision detection)
  LET $position_used = 0
  
  # Check if position is off-screen
  IF $x < 0 THEN
    LET $position_used = 1
  ENDIF
  IF $y < 0 THEN
    LET $position_used = 1
  ENDIF
  
  LET $max_x = $WIDTH - $eye_size
  IF $x > $max_x THEN
    LET $position_used = 1
  ENDIF
  
  LET $max_y = $HEIGHT - $eye_size
  IF $y > $max_y THEN
    LET $position_used = 1
  ENDIF
  
  # Only draw if position is valid
  IF $position_used == 0 THEN
    # Apply a random rotation occasionally
    LET $seed_temp = $seed * $multiplier
    LET $seed_temp2 = $seed_temp + $increment
    LET $seed = $seed_temp2 % $modulus
    
    LET $rotate_chance = $seed % 4
    
    VAR $size = 6 + $seed % 6
    
    IF $rotate_chance == 0 THEN
      LET $seed_temp = $seed * $multiplier
      LET $seed_temp2 = $seed_temp + $increment
      LET $seed = $seed_temp2 % $modulus
      
      LET $rotation = $seed % 360
      RESET_TRANSFORMS
      TRANSLATE DX=$x DY=$y
      ROTATE DEGREES=$rotation
      
      SCALE FACTOR=$size
      
      IF $eye_type == 1 THEN
        DRAW NAME="eye1" X=0 Y=0
      ENDIF
      IF $eye_type == 2 THEN
        DRAW NAME="eye2" X=0 Y=0
      ENDIF
      IF $eye_type == 3 THEN
        DRAW NAME="eye3" X=0 Y=0
      ENDIF
      
      RESET_TRANSFORMS
    

    ELSE
      # Draw without rotation 
       SCALE FACTOR=$size
        
      IF $eye_type == 1 THEN
        DRAW NAME="eye1" X=$x Y=$y
      ENDIF
      IF $eye_type == 2 THEN
        DRAW NAME="eye2" X=$x Y=$y
      ENDIF
      IF $eye_type == 3 THEN
        DRAW NAME="eye3" X=$x Y=$y
      ENDIF
    ENDIF
  ENDIF
ENDREPEAT)MPS";

static const char SCRIPT_S0[] = R"MPS(# City Map Generator - MicroPatterns DSL
# Creates a procedural city layout using 8 different 20x20 tile patterns
# Adapts to any screen size using $WIDTH and $HEIGHT

# Define 8 city tile patterns (20x20 each)

# Pattern 1: Empty lot/grass

DEFINE PATTERN NAME="empty" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111000000011111111111110000001111111111111000000011111100000000000000111000000000000000001110001111111111100011111111111111111100111111111111111111000111110000000001110000000000000000011100000000000001111111000000000000011111110000000000000111000000000000"


DEFINE PATTERN NAME="building" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011100111111111111111111101111111111111111111001111111111111100110000000000000000001100000000000000001111001111111111111111110011111111111111111001111111111111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"


# Pattern 3: Horizontal road
DEFINE PATTERN NAME="road_h" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111111111111111111111111111111111111111110111111111111100000000000000111000000000000000001110001111111000000011111111111111000000111111111111110000000111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"

# Pattern 4: Vertical road
DEFINE PATTERN NAME="road_v" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011100111111111111111111101111111111111111111001111111111111100110000000000000000001100000000000000001111001111111111111111110011111111111111111001111111111111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"

# Pattern 5: Intersection (cross roads)
DEFINE PATTERN NAME="intersection" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100011111100000000111001111111100000001110011100111000000011100111001110011111111111110011111111111111111100111111111111111111001111100000111001110011100000001110011100111001111111111111111111111111111111111111111111111111111111111110000011100111001110000000111001110011100000001110011111111000000011100011111100000000111000000000000"

# Pattern 6: Park/green space
DEFINE PATTERN NAME="park" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111000011111111111111110001111111111111111000011111111100000000000111000000000000000001110000001111111111111111111111111111111111111111111111111111111111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"

# Pattern 7: Dense building area
DEFINE PATTERN NAME="dense" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111001111111111111111110111111111111111111001111111111100000000011000000000000000000110000000001111111111100111111111111111111011111111111111111100111111110000000000001110000000000000000011100000000000111111111000000000011111111110000000000111000000000000"

# Pattern 8: Commercial strip
DEFINE PATTERN NAME="commercial" WIDTH=20 HEIGHT=20 DATA="0000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000000001110000000000000000011100000000000011111111001111111111111111100111111111111111110001111111111100000000011100000000000000000111000000001111110001111111111111111110011111111111111111110011111111110000011100000000000000000111000000000000000001110000000000000000011100000000000000000111000000000000"

# Variables for city generation with more randomness sources
VAR $seed = $HOUR * 60 + $MINUTE + $COUNTER
VAR $time_factor = $SECOND * 3 + $MINUTE / 10
VAR $counter_mod = $COUNTER % 17

VAR $scale_random = $seed + $time_factor * 7 + $counter_mod * 11
VAR $scaling = 2 + $scale_random % 11

VAR $tilewidth = 20 * $scaling
# Calculate grid dimensions (how many 20x20 scaled up tiles fit)
# add 1 to account for partial grids, avoid white borders
VAR $grid_width = 1 + $WIDTH / $tilewidth
VAR $grid_height = 1 + $HEIGHT / $tilewidth

# Ensure minimum grid size
IF $grid_width < 1 THEN
    LET $grid_width = 1
ENDIF
IF $grid_height < 1 THEN
    LET $grid_height = 1
ENDIF

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
ENDREPEAT)MPS";

static const EmbeddedScript EMBEDDED_SCRIPTS[] = {
    { "reconnected", "Re/Connected", SCRIPT_S4 },
    { "thunderstorms", "Thunderstorms", SCRIPT_S5 },
    { "eyes", "Eyes", SCRIPT_S3 },
    { "circuits", "Circuits", SCRIPT_S0 },
};
static const int EMBEDDED_SCRIPT_COUNT = sizeof(EMBEDDED_SCRIPTS)/sizeof(EMBEDDED_SCRIPTS[0]);

#endif // EMBEDDED_SCRIPTS_H