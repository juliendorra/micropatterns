# Regression gate for two bounds bugs found 2026-09-03. Both were invisible to
# every other corpus script, and one of them rendered NOTHING AT ALL -- which is
# exactly the kind of failure a golden can only catch if some script provokes it.
#
# 1. An axis-aligned LINE has an AABB of zero thickness. floor/ceil collapsed it
#    to minY == maxY, the "clipped away to nothing" test read that as off-screen,
#    and the item was culled. EVERY horizontal and vertical LINE drew nothing.
#    Measured: ROTATE 0 / 90 / 180 gave 0 px, ROTATE 1 / 89 / 91 gave all 401.
#    prims.mp draws lines only inside a ROTATE $i loop, so it never hit 0/90/180.
#
# 2. FILL_CIRCLE bounded itself with the AABB of an INSCRIBED OCTAGON -- eight
#    sampled points, four of them via a 0.7071 diagonal offset -- which
#    undershoots a rotated disk by r*(1 - cos 22.5deg) = 7.6% of the radius.
#    Measured on radius 200: 400 px wide at ROTATE 0, clipped to 372 px at
#    ROTATE 22, losing 4.4% of the area and drawing eight flat sides.
#
# Deliberately static: it must render the same at every seed, so a failure here
# is unambiguously a renderer change and never a clock or counter difference.

COLOR NAME=BLACK

# --- 1. axis-aligned lines, untransformed --------------------------------
RESET_TRANSFORMS
LINE X1=20 Y1=40 X2=320 Y2=40
LINE X1=40 Y1=60 X2=40 Y2=360

# --- 1b. axis-aligned lines reached THROUGH a rotation -------------------
# The transform, not the operands, is what makes these axis-aligned on screen.
RESET_TRANSFORMS
TRANSLATE DX=200 DY=460
ROTATE DEGREES=90
LINE X1=-150 Y1=0 X2=150 Y2=0

RESET_TRANSFORMS
TRANSLATE DX=420 DY=180
ROTATE DEGREES=180
LINE X1=-120 Y1=0 X2=120 Y2=0

RESET_TRANSFORMS
TRANSLATE DX=420 DY=300
ROTATE DEGREES=270
LINE X1=-100 Y1=0 X2=100 Y2=0

# A zero-length LINE is degenerate in BOTH axes at once.
RESET_TRANSFORMS
LINE X1=500 Y1=500 X2=500 Y2=500

# --- 2. rotated filled disks, at and around the worst-case angle ---------
# 22 degrees is where the octagon AABB undershot hardest.
RESET_TRANSFORMS
TRANSLATE DX=640 DY=180
ROTATE DEGREES=22
FILL_CIRCLE X=0 Y=0 RADIUS=120

RESET_TRANSFORMS
TRANSLATE DX=640 DY=400
ROTATE DEGREES=67
SCALE FACTOR=2
FILL_CIRCLE X=0 Y=0 RADIUS=55

# The outline path computed its extent correctly all along; drawn concentric
# with a filled disk so a future divergence between the two is visible.
RESET_TRANSFORMS
TRANSLATE DX=870 DY=270
ROTATE DEGREES=22
FILL_CIRCLE X=0 Y=0 RADIUS=60
COLOR NAME=WHITE
CIRCLE X=0 Y=0 RADIUS=60
