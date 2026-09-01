# Integer-width probe. 100000*100000 = 10,000,000,000, which does not fit in
# int32. A 32-bit engine wraps to 1,410,065,408; a double-based engine keeps the
# exact value. Taking % 97 turns that into a visible x position: 76 vs 49.
VAR $a = 100000
LET $a = $a * 100000
LET $a = $a % 97
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT X=$a Y=10 WIDTH=4 HEIGHT=4

# Second case: negative wrap. 2000000000 + 2000000000 overflows to a NEGATIVE
# int32 on the device. A double-based engine gets 4e9 and stays positive, so the
# two disagree about whether the rect is drawn at all.
VAR $b = 2000000000
LET $b = $b + $b
LET $b = $b % 211
LET $b = $b + 300
FILL_RECT X=$b Y=30 WIDTH=4 HEIGHT=4
