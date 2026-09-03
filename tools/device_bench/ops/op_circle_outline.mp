# CONTROL GROUP. One transformed centre, one radius, then the integer midpoint
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
