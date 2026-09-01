# Integer-width probe. 100000*100000 = 10,000,000,000, which does not fit in
# int32. A 32-bit engine wraps to 1,410,065,408; a double-based engine keeps the
# exact value. Taking % 97 turns that into a visible x position: 76 vs 49.
VAR $a = 100000
LET $a = $a * 100000
LET $a = $a % 97
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT X=$a Y=10 WIDTH=4 HEIGHT=4
