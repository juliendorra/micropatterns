
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
                    
        