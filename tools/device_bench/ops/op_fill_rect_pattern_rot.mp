# The same patterned fill rotated, so im1 != 0 and the row hoist is impossible:
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
