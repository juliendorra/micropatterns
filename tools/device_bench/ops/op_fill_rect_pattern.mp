# Patterned FILL_RECT, unrotated: im1 == 0, so the rasteriser hoists the pattern
# ROW out of the scanline and transforms only x per pixel.
DEFINE PATTERN NAME="p4" WIDTH=4 HEIGHT=4 DATA="1000010000100001"
COLOR NAME=BLACK
FILL NAME="p4"
RESET_TRANSFORMS
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
