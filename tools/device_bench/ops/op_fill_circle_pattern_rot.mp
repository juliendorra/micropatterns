# Rotated patterned disk: the disk test plus the un-hoistable per-pixel
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
