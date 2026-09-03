# DRAW of a scaled-up asset. A separate path from FILL_*: it walks the asset's
# own pixels rather than a screen-space span, so the per-pixel cost is a
# different loop over the same transform.
DEFINE PATTERN NAME="a8" WIDTH=8 HEIGHT=8 DATA="1100110011001100001100110011001111001100110011000011001100110011"
VAR $s = 0
COLOR NAME=BLACK
LET $s = $WIDTH / 8
RESET_TRANSFORMS
SCALE FACTOR=$s
DRAW NAME="a8" X=0 Y=0
