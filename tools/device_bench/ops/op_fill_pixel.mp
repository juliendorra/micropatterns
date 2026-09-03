# FILL_PIXEL one at a time: the most transform-heavy path per painted pixel.
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
