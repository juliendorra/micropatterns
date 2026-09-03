# CONTROL GROUP. Four transformed corners, then four integer Bresenham runs.
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
