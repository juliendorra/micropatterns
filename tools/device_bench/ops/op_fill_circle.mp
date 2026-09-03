# Solid disk inscribed in the canvas. Adds the exact disk test
# dx*dx + dy*dy <= r*r and the narrowSpan pass on top of the plain fill.
VAR $r = 0
COLOR NAME=BLACK
FILL NAME=SOLID
LET $r = $WIDTH / 2
RESET_TRANSFORMS
FILL_CIRCLE X=$r Y=$r RADIUS=$r
