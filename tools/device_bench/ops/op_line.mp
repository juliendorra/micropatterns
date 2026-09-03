# CONTROL GROUP. Bresenham already runs on integers; a fixed-point transform
# port must not move this number. Only the two endpoints are transformed.
VAR $i = 0
COLOR NAME=BLACK
REPEAT COUNT=40
  LET $i = $INDEX
  RESET_TRANSFORMS
  LINE X1=0 Y1=$i X2=$WIDTH Y2=$HEIGHT
ENDREPEAT
