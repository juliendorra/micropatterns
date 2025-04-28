# MicroPatterns Project

This project defines and implements the **MicroPatterns DSL**, a mini-language designed for creating generative pixel art, especially for monochrome e-ink displays on devices like the ESP32 (e.g., Watchy).

It includes:
*   The MicroPatterns DSL Specification (see below).
*   A JavaScript-based emulator (`micropatterns_emulator/`) for testing and developing scripts in a web browser.
*   (Future) A C++/Arduino implementation for target devices.

## MicroPatterns DSL Overview

MicroPatterns allows you to define simple rules and use environment variables (time, counter) to generate evolving abstract or representational pixel art. Its key features include:

*   **Simplicity:** Easy-to-understand keywords and syntax.
*   **Integer Math:** Optimized for resource-constrained devices.
*   **State Machine:** Drawing state (color, transforms, patterns) is set sequentially.
*   **E-ink Friendly:** Focus on monochrome output and pattern fills.
*   **Generative:** Uses time (`$HOUR`, `$MINUTE`, `$SECOND`) and a `$COUNTER` for variation.
*   **Assets:** Define reusable `PATTERN` and `ICON` bitmaps within the script.
*   **Transformations:** `TRANSLATE`, `ROTATE`, `SCALE` commands modify subsequent drawing.
*   **Control Flow:** Basic `REPEAT` loops and `IF/ELSE/ENDIF` conditionals.

---

## MicroPatterns DSL Specification v1.0

### Overview

MicroPatterns is a mini-language designed for creating generative pixel art, primarily targeting resource-constrained devices like ESP32-based e-ink displays (e.g., Watchy). It generates static images based on environment variables (time, counter) and user-defined assets (patterns, icons).

### Core Concepts

1.  **Environment Driven:** Generation uses read-only variables:
    *   `$HOUR` (0-23)
    *   `$MINUTE` (0-59)
    *   `$SECOND` (0-59)
    *   `$COUNTER` (Increments each run, starts at 0)
    *   `$WIDTH` (Display width, e.g., 200)
   *   `$HEIGHT` (Display height, e.g., 200)
2.  **Integer Math:** All coordinates, parameters, and calculations use integers. Division truncates towards zero.
3.  **State Machine:** Commands like `COLOR`, `TRANSLATE`, `ROTATE`, `SCALE`, `PATTERN` modify the drawing state for subsequent commands.
4.  **Case Sensitivity:**
   *   Keywords (e.g., `COLOR`, `PIXEL`) and parameter names (e.g., `X=`, `NAME=`) are **case-insensitive**.
   *   Variable names (`myVar`), Pattern names (`"myPattern"`), and Icon names (`"myIcon"`) are **case-insensitive**. They are treated consistently regardless of the case used in definition or reference (e.g., `VAR myVar` is the same as `LET MYVAR = ...`). The emulator internally converts them to uppercase.
   *   Environment variable names (`$HOUR`, `$SECOND`, etc.) are also treated as **case-insensitive** (typically referenced as uppercase).
   *   String literal *content* (e.g., the `"pattern_name"` itself inside quotes) remains case-sensitive if the underlying system requires it, but the *identifier* lookup is case-insensitive.
5.  **Named Parameters:** All commands taking arguments require named parameters (e.g., `PIXEL X=10 Y=20`).
6.  **E-ink Focus:** Assumes a monochrome (Black/White) display. Pattern fills are a key feature.
7.  **External Assets:** Icons and Patterns are defined within the script using `DEFINE`.

### Syntax

*   **Comments:** Start with `#` and continue to the end of the line.
*   **Commands:** One command per line.
*   **Parameters:** `NAME=value` format. Order generally doesn't matter unless specified. Values can be integer literals, string literals (in double quotes), variable references (`$variable_name` - case-insensitive), or environment variables (`$HOUR`, etc. - case-insensitive).

### Environment Variables (Read-Only)

*   `$HOUR`: Current hour (0-23)
*   `$MINUTE`: Current minute (0-59)
*   `$SECOND`: Current second (0-59)
*   `$COUNTER`: Script execution counter (starts at 0, increments each run)
*   `$WIDTH`: Display width in pixels
*   `$HEIGHT`: Display height in pixels
*   *(Note: Access is case-insensitive, e.g., `$hour` works but resolves to the value of `$HOUR`)*

### Variables

*   **Declaration:**
   ```micropatterns
   VAR variable_name
   ```
   *   Declares a variable. Initial value is 0. Variable names are **case-insensitive**. Cannot conflict with environment variable names (case-insensitively).

*   **Assignment:**
   ```micropatterns
   LET variable_name = expression
   ```
   *   Assigns the result of an expression to a *previously declared* variable (case-insensitive lookup).
   *   **Expressions:** Limited to integer math: `value [+-*/%] value ...`
       *   `value` can be an integer literal, `$variable` (case-insensitive), or an environment variable (`$HOUR`, etc. - case-insensitive).
       *   Operations are performed with standard precedence (`*`, `/`, `%` before `+`, `-`) and left-to-right associativity for equal precedence.
       *   Parentheses are **NOT** supported.
       *   Division `/` is integer division (truncates towards zero).
       *   Modulo `%` gives the remainder. Division/Modulo by zero is a runtime error.
   *   Example: `LET angle = $minute * 6` (uses `$MINUTE`)
   *   Example: `LET offset = $counter % 20 + 10` (uses `$COUNTER`)

### Asset Definition

*   **Patterns:**
   ```micropatterns
   DEFINE PATTERN NAME="pattern_name" WIDTH=w HEIGHT=h DATA="0110..."
   ```
   *   Defines a reusable monochrome pattern. Must appear before usage.
   *   `NAME`: Identifier string (in double quotes). The identifier itself is **case-insensitive** for definition and lookup (e.g., `DEFINE PATTERN NAME="Checker"` is the same as `PATTERN NAME="checker"`).
   *   `WIDTH`, `HEIGHT`: Dimensions (integers, 1-20 recommended).
   *   `DATA`: String of '0' (white/transparent) and '1' (draw color) characters, row by row. Length must equal `WIDTH * HEIGHT`.
   *   Maximum of 8 patterns can be defined per script.

*   **Icons:**
   ```micropatterns
   DEFINE ICON NAME="icon_name" WIDTH=w HEIGHT=h DATA="0110..."
   ```
   *   Defines a reusable monochrome icon bitmap. Must appear before usage.
   *   Same parameters and constraints as `DEFINE PATTERN`. Identifier `NAME` is **case-insensitive**.
   *   Maximum of 16 icons can be defined per script.

### Drawing State Commands

*   **Color:**
   ```micropatterns
   COLOR NAME=BLACK
   COLOR NAME=WHITE
   ```
   *   Sets the current drawing color for subsequent primitives, fills, and icons. Default is `BLACK`. `NAME` value is case-insensitive.

*   **Pattern:**
   ```micropatterns
   PATTERN NAME="pattern_name"
   PATTERN NAME=SOLID
   ```
   *   Sets the fill pattern for `FILL_RECT` and `FILL_CIRCLE`.
   *   `NAME="pattern_name"`: Uses a previously defined pattern (identifier lookup is **case-insensitive**).
   *   `NAME=SOLID`: Resets to solid fill using the current `COLOR`. Default is `SOLID`. `SOLID` keyword is case-insensitive.

### Transformation Commands

*   Transformations apply cumulatively to subsequent drawing commands (`PIXEL`, `LINE`, `RECT`, `FILL_RECT`, `CIRCLE`, `FILL_CIRCLE`, `ICON`).
*   The origin for rotation and scaling is the current (0,0) point *after* translation.
*   Order of application for a point (x,y): Scale -> Rotate -> Translate.
*   Parameters (`DX`, `DY`, `DEGREES`, `FACTOR`) can be integer literals or variables (`$var` - case-insensitive).

*   **Reset:**
   ```micropatterns
   RESET_TRANSFORMS
   ```
   *   Resets translation to (0,0), rotation to 0 degrees, and scale to 1. Takes no parameters.

*   **Translate:**
   ```micropatterns
   TRANSLATE DX=tx DY=ty
   ```
   *   Adds (`tx`, `ty`) to the current translation offset.

*   **Rotate:**
   ```micropatterns
   ROTATE DEGREES=d
   ```
   *   Sets the *absolute* rotation angle around the current origin. `d` is an integer (0-359, wraps around). Uses integer math internally (e.g., precomputed sin/cos tables). Replaces any previous rotation.

*   **Scale:**
   ```micropatterns
   SCALE FACTOR=f
   ```
   *   Sets the *absolute* uniform scaling factor. `f` is an integer >= 1. `FACTOR=1` is normal size. Replaces any previous scale.

### Drawing Primitives

*   Coordinates (`X`, `Y`, `X1`, `Y1`, etc.) and dimensions (`WIDTH`, `HEIGHT`, `RADIUS`) are affected by the current transformation state as described below.
*   Parameters can be integer literals or variables (`$var` - case-insensitive).

*   **Pixel:**
   ```micropatterns
   PIXEL X=x Y=y
   ```
   *   Draws a single pixel at the transformed `(x,y)` in the current `COLOR`.

*   **Line:**
   ```micropatterns
   LINE X1=x1 Y1=y1 X2=x2 Y2=y2
   ```
   *   Draws a line between transformed `(x1,y1)` and `(x2,y2)` using Bresenham's algorithm in the current `COLOR`.

*   **Rectangle Outline:**
   ```micropatterns
   RECT X=x Y=y WIDTH=w HEIGHT=h
   ```
   *   Draws the outline of a rectangle. The four corner points `(x,y)`, `(x+w, y)`, `(x+w, y+h)`, `(x, y+h)` are transformed, and lines are drawn between them in the current `COLOR`.

*   **Filled Rectangle:**
   ```micropatterns
   FILL_RECT X=x Y=y WIDTH=w HEIGHT=h
   ```
   *   Draws a filled rectangle. Fill uses the current `PATTERN` (tiled, case-insensitive lookup) or `COLOR` (if `PATTERN NAME=SOLID`). The fill area is determined by the transformed rectangle shape. (Note: Pattern fill on rotated rectangles may be approximated in some implementations).

*   **Circle Outline:**
   ```micropatterns
   CIRCLE X=x Y=y RADIUS=r
   ```
   *   Draws the outline of a circle centered at transformed `(x,y)` with radius `r` in the current `COLOR`. Uses Midpoint/Bresenham circle algorithm. The radius `r` is multiplied by the current `SCALE` factor.

*   **Filled Circle:**
   ```micropatterns
   FILL_CIRCLE X=x Y=y RADIUS=r
   ```
   *   Draws a filled circle. Fill uses the current `PATTERN` (case-insensitive lookup) or `COLOR`. The radius `r` is multiplied by the current `SCALE` factor.

*   **Icon:**
   ```micropatterns
   ICON NAME="icon_name" X=x Y=y
   ```
   *   Draws the previously defined icon named `"icon_name"` (identifier lookup is **case-insensitive**).
   *   The icon's local top-left corner `(0,0)` is placed at the transformed `(x,y)`.
   *   The icon's pixels are then individually transformed relative to this origin using the current `ROTATE` and `SCALE` state.
   *   Draws using the current `COLOR` for '1' pixels in the icon data.

### Control Flow

*   **Repeat Loop:**
   ```micropatterns
   REPEAT COUNT=n TIMES
       # Commands to repeat...
       # $INDEX is available here (0 to n-1)
   ENDREPEAT
   ```
   *   Executes the block of commands `n` times. `n` can be an integer literal or variable (`$var` - case-insensitive), must be >= 0.
   *   An implicit, read-only variable `$INDEX` (case-insensitive lookup) holds the current iteration number (0-based) within the loop block.

*   **Conditional:**
   ```micropatterns
   IF condition THEN
       # Commands if condition is true...
   ELSE
       # Optional: Commands if condition is false...
   ENDIF
   ```
   *   Executes the `THEN` block if the `condition` is true.
   *   If `ELSE` is present, executes the `ELSE` block if the `condition` is false. `ELSE` must appear immediately after the `THEN` block.
   *   `ENDIF` marks the end of the conditional block.
   *   **Conditions:**
       *   `value1 operator value2`
           *   `value` can be an integer literal, `$variable` (case-insensitive), or an environment variable (case-insensitive).
           *   `operator` can be: `==` (equals), `!=` (not equals), `>` (greater), `<` (less), `>=` (greater or equal), `<=` (less or equal).
       *   Special modulo condition: `$variable % literal operator value`
           *   Example: `$counter % 10 == 0` (uses `$COUNTER`)
           *   `literal` must be a positive integer literal. `value` can be integer literal or variable (case-insensitive).

### Example Script

```micropatterns
# Define assets first (names are case-insensitive)
DEFINE PATTERN NAME="Checker" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="stripes" WIDTH=4 HEIGHT=4 DATA="1111000011110000"
DEFINE ICON NAME="Arrow" WIDTH=5 HEIGHT=8 DATA="00100011100010001111100100001000010000100"

# Variables (names are case-insensitive)
VAR Angle
VAR RADIUS

# --- Calculations based on time ---
LET angle = $MINUTE * 6  # Angle for minute hand (uses $MINUTE)
LET radius = 10 + ($second % 10) * 2 # Radius pulsates slightly (uses $SECOND)

# --- Draw patterned background based on counter ---
RESET_TRANSFORMS
IF $COUNTER % 2 == 0 THEN
   PATTERN NAME="checker" # Case-insensitive lookup
ELSE
   PATTERN NAME="Stripes" # Case-insensitive lookup
ENDIF
FILL_RECT X=0 Y=0 WIDTH=$width HEIGHT=$HEIGHT # Fill whole screen (uses $WIDTH, $HEIGHT)

# --- Draw Hour Markers ---
COLOR NAME=BLACK
PATTERN NAME=SOLID # Use solid color for markers
REPEAT COUNT=12 TIMES
   RESET_TRANSFORMS
   TRANSLATE DX=100 DY=100 # Center origin (assuming 200x200 display)
   ROTATE DEGREES=($index * 30) # Rotate per hour (uses $INDEX)
   TRANSLATE DX=0 DY=-90 # Move out
   FILL_RECT X=-2 Y=-5 WIDTH=4 HEIGHT=10 # Draw marker
ENDREPEAT

# --- Draw pulsating circle ---
RESET_TRANSFORMS
TRANSLATE DX=100 DY=100
COLOR NAME=WHITE # Draw on top of pattern
PATTERN NAME=SOLID
FILL_CIRCLE X=0 Y=0 RADIUS=$radius # Uses variable RADIUS (case-insensitive)

# --- Draw Minute Hand (Icon) ---
RESET_TRANSFORMS
TRANSLATE DX=100 DY=100
ROTATE DEGREES=$Angle # Uses variable Angle (case-insensitive)
# SCALE FACTOR=2 # Example: Make icon larger
COLOR NAME=BLACK
ICON NAME="arrow" X=-2 Y=-30 # Position icon (case-insensitive lookup)

# --- Draw Counter Value ---
# (Simple example: draw pixels based on counter)
RESET_TRANSFORMS
COLOR NAME=WHITE
VAR x_pos
VAR y_pos
LET x_pos = $counter % $WIDTH # Uses $COUNTER, $WIDTH
LET y_pos = ($COUNTER / $width) % $HEIGHT # Integer division wraps y (uses $COUNTER, $WIDTH, $HEIGHT)
PIXEL X=$x_pos Y=$y_pos # Uses variables x_pos, y_pos
```
    ```
    *   Resets translation to (0,0), rotation to 0 degrees, and scale to 1. Takes no parameters.

*   **Translate:**
    ```micropatterns
    TRANSLATE DX=tx DY=ty
    ```
    *   Adds (`tx`, `ty`) to the current translation offset.

*   **Rotate:**
    ```micropatterns
    ROTATE DEGREES=d
    ```
    *   Sets the *absolute* rotation angle around the current origin. `d` is an integer (0-359, wraps around). Uses integer math internally (e.g., precomputed sin/cos tables). Replaces any previous rotation.

*   **Scale:**
    ```micropatterns
    SCALE FACTOR=f
    ```
    *   Sets the *absolute* uniform scaling factor. `f` is an integer >= 1. `FACTOR=1` is normal size. Replaces any previous scale.

### Drawing Primitives

*   Coordinates (`X`, `Y`, `X1`, `Y1`, etc.) and dimensions (`WIDTH`, `HEIGHT`, `RADIUS`) are affected by the current transformation state as described below.
*   Parameters can be integer literals or variables (`$var`).

*   **Pixel:**
    ```micropatterns
    PIXEL X=x Y=y
    ```
    *   Draws a single pixel at the transformed `(x,y)` in the current `COLOR`.

*   **Line:**
    ```micropatterns
    LINE X1=x1 Y1=y1 X2=x2 Y2=y2
    ```
    *   Draws a line between transformed `(x1,y1)` and `(x2,y2)` using Bresenham's algorithm in the current `COLOR`.

*   **Rectangle Outline:**
    ```micropatterns
    RECT X=x Y=y WIDTH=w HEIGHT=h
    ```
    *   Draws the outline of a rectangle. The four corner points `(x,y)`, `(x+w, y)`, `(x+w, y+h)`, `(x, y+h)` are transformed, and lines are drawn between them in the current `COLOR`.

*   **Filled Rectangle:**
    ```micropatterns
    FILL_RECT X=x Y=y WIDTH=w HEIGHT=h
    ```
    *   Draws a filled rectangle. Fill uses the current `PATTERN` (tiled) or `COLOR` (if `PATTERN NAME=SOLID`). The fill area is determined by the transformed rectangle shape. (Note: Pattern fill on rotated rectangles may be approximated in some implementations).

*   **Circle Outline:**
    ```micropatterns
    CIRCLE X=x Y=y RADIUS=r
    ```
    *   Draws the outline of a circle centered at transformed `(x,y)` with radius `r` in the current `COLOR`. Uses Midpoint/Bresenham circle algorithm. The radius `r` is multiplied by the current `SCALE` factor.

*   **Filled Circle:**
    ```micropatterns
    FILL_CIRCLE X=x Y=y RADIUS=r
    ```
    *   Draws a filled circle. Fill uses the current `PATTERN` or `COLOR`. The radius `r` is multiplied by the current `SCALE` factor.

*   **Icon:**
    ```micropatterns
    ICON NAME="icon_name" X=x Y=y
    ```
    *   Draws the previously defined icon named `"icon_name"` (case-sensitive string literal).
    *   The icon's local top-left corner `(0,0)` is placed at the transformed `(x,y)`.
    *   The icon's pixels are then individually transformed relative to this origin using the current `ROTATE` and `SCALE` state.
    *   Draws using the current `COLOR` for '1' pixels in the icon data.

### Control Flow

*   **Repeat Loop:**
    ```micropatterns
    REPEAT COUNT=n TIMES
        # Commands to repeat...
        # $INDEX is available here (0 to n-1)
    ENDREPEAT
    ```
    *   Executes the block of commands `n` times. `n` can be an integer literal or variable (`$var`), must be >= 0.
    *   An implicit, read-only variable `$INDEX` holds the current iteration number (0-based) within the loop block.

*   **Conditional:**
    ```micropatterns
    IF condition THEN
        # Commands if condition is true...
    ELSE
        # Optional: Commands if condition is false...
    ENDIF
    ```
    *   Executes the `THEN` block if the `condition` is true.
    *   If `ELSE` is present, executes the `ELSE` block if the `condition` is false. `ELSE` must appear immediately after the `THEN` block.
    *   `ENDIF` marks the end of the conditional block.
    *   **Conditions:**
        *   `value1 operator value2`
            *   `value` can be an integer literal, `$variable`, or an environment variable.
            *   `operator` can be: `==` (equals), `!=` (not equals), `>` (greater), `<` (less), `>=` (greater or equal), `<=` (less or equal).
        *   Special modulo condition: `$variable % literal operator value`
            *   Example: `$COUNTER % 10 == 0`
            *   `literal` must be a positive integer literal. `value` can be integer literal or variable.

### Example Script

```micropatterns
# Define assets first
DEFINE PATTERN NAME="checker" WIDTH=4 HEIGHT=4 DATA="1010010110100101"
DEFINE PATTERN NAME="stripes" WIDTH=4 HEIGHT=4 DATA="1111000011110000"
DEFINE ICON NAME="arrow" WIDTH=5 HEIGHT=8 DATA="00100011100010001111100100001000010000100"

# Variables
VAR angle
VAR radius

# --- Calculations based on time ---
LET angle = $MINUTE * 6  # Angle for minute hand
LET radius = 10 + ($SECOND % 10) * 2 # Radius pulsates slightly

# --- Draw patterned background based on counter ---
RESET_TRANSFORMS
IF $COUNTER % 2 == 0 THEN
    PATTERN NAME="checker"
ELSE
    PATTERN NAME="stripes"
ENDIF
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT # Fill whole screen

# --- Draw Hour Markers ---
COLOR NAME=BLACK
PATTERN NAME=SOLID # Use solid color for markers
REPEAT COUNT=12 TIMES
    RESET_TRANSFORMS
    TRANSLATE DX=100 DY=100 # Center origin (assuming 200x200 display)
    ROTATE DEGREES=($INDEX * 30) # Rotate per hour
    TRANSLATE DX=0 DY=-90 # Move out
    FILL_RECT X=-2 Y=-5 WIDTH=4 HEIGHT=10 # Draw marker
ENDREPEAT

# --- Draw pulsating circle ---
RESET_TRANSFORMS
TRANSLATE DX=100 DY=100
COLOR NAME=WHITE # Draw on top of pattern
PATTERN NAME=SOLID
FILL_CIRCLE X=0 Y=0 RADIUS=$radius

# --- Draw Minute Hand (Icon) ---
RESET_TRANSFORMS
TRANSLATE DX=100 DY=100
ROTATE DEGREES=$angle
# SCALE FACTOR=2 # Example: Make icon larger
COLOR NAME=BLACK
ICON NAME="arrow" X=-2 Y=-30 # Position icon (adjust X based on icon width)

# --- Draw Counter Value ---
# (Simple example: draw pixels based on counter)
RESET_TRANSFORMS
COLOR NAME=WHITE
VAR x_pos
VAR y_pos
LET x_pos = $COUNTER % $WIDTH
LET y_pos = ($COUNTER / $WIDTH) % $HEIGHT # Integer division wraps y
PIXEL X=$x_pos Y=$y_pos
```