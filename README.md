# MicroPatterns Project

This project defines and implements the **MicroPatterns DSL**, a mini-language designed for creating generative pixel art, targeting monochrome e-ink displays on devices like the ESP32-based M5Paper and Watchy clones.

It includes:
*   The MicroPatterns DSL Specification (see below).
*   A JavaScript-based emulator (`micropatterns_emulator/`) for testing and developing scripts in a web browser.
*   An ESP32 C++/Arduino runtime for M5Paper, including:
    *   Parsing and execution of the DSL.
    *   SPIFFS storage for multiple scripts.
    *   Fetching scripts from a remote API (e.g., `micropatterns-api.deno.dev`).
    *   Automatic refresh checks only on boot and after timer-based wakeups (not on button presses).
    *   Graceful handling of unavailable WiFi - skips fetch entirely if WiFi is unavailable.
    *   Non-blocking fetch operations that never prevent device operation.
    *   Light sleep support for power saving.
    *   Graceful handling of unavailable WiFi - skips fetch entirely if WiFi is unavailable.
    *   Non-blocking fetch operations that never prevent device operation.
    *   Light sleep support for power saving.

## MicroPatterns DSL Overview

MicroPatterns allows you to define simple rules and use environment variables (time, counter) to generate evolving abstract or representational pixel art. Its key features include:

*   **Simplicity:** Easy-to-understand keywords and syntax.
*   **Integer Math:** Optimized for resource-constrained devices.
*   **State Machine:** Drawing state (color, transforms, fill pattern) is set sequentially.
*   **E-ink Friendly:** Focus on monochrome output and pattern fills.
*   **Generative:** Uses time (`$HOUR`, `$MINUTE`, `$SECOND`) and a `$COUNTER` for variation.
*   **Patterns:** Define reusable `PATTERN` bitmaps within the script using `DEFINE PATTERN`. These patterns can then be used for filling areas (`FILL`) or drawing directly (`DRAW`).
*   **Transformations:** `TRANSLATE`, `ROTATE`, `SCALE` commands modify subsequent drawing.
*   **Control Flow:** Basic `REPEAT` loops and `IF/ELSE/ENDIF` conditionals.
*   **Pattern-Based Drawing:** Draw individual pixels conditionally based on the current fill pattern using `FILL_PIXEL`.

---

## MicroPatterns DSL Specification v1.3 

1.2 FILL_PIXEL
1.3 Remove TIMES from REPEAT

### Overview

MicroPatterns is a mini-language designed for creating generative pixel art, primarily targeting resource-constrained devices like ESP32-based e-ink displays (e.g., Watchy). It generates static images based on environment variables (time, counter) and user-defined patterns.

### Core Concepts

1.  **Environment Driven:** Generation uses read-only variables:
    *   `$HOUR` (0-23)
    *   `$MINUTE` (0-59)
    *   `$SECOND` (0-59)
    *   `$COUNTER` (Increments each run, starts at 0)
    *   `$WIDTH` (Display width, e.g., 200)
   *   `$HEIGHT` (Display height, e.g., 200)
2.  **Integer Math:** All coordinates, parameters, and calculations use integers. Division truncates towards zero.
3.  **State Machine:** Commands like `COLOR`, `TRANSLATE`, `ROTATE`, `SCALE`, `FILL` modify the drawing state for subsequent commands.
4.  **Case Sensitivity:**
   *   Keywords (e.g., `COLOR`, `PIXEL`, `DEFINE`, `PATTERN`, `FILL`, `DRAW`) and parameter names (e.g., `X=`, `NAME=`) are **case-insensitive**.
   *   Variable names (`myVar`) and Pattern names (`"myPattern"`) are **case-insensitive**. They are treated consistently regardless of the case used in definition or reference (e.g., `VAR myVar` is the same as `LET MYVAR = ...`, `DEFINE PATTERN NAME="Checker"` is the same as `FILL NAME="checker"` or `DRAW NAME="checker"`). The emulator internally converts them to uppercase.
   *   Environment variable names (`$HOUR`, `$SECOND`, etc.) are also treated as **case-insensitive** (typically referenced as uppercase).
   *   String literal *content* (e.g., the `"pattern_name"` itself inside quotes) remains case-sensitive if the underlying system requires it, but the *identifier* lookup is case-insensitive.
5.  **Named Parameters:** All commands taking arguments require named parameters (e.g., `PIXEL X=10 Y=20`).
6.  **E-ink Focus:** Assumes a monochrome (Black/White) display. Pattern fills are a key feature.
7.  **Patterns:** Patterns are defined within the script using `DEFINE PATTERN` and can be used for filling (`FILL`), direct drawing (`DRAW`), or conditional pixel drawing (`FILL_PIXEL`).

### Syntax

*   **Comments:** Start with `#` and continue to the end of the line. Comments must be on their own line, end of line comments are not valid.
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

*   **Declaration & Optional Initialization:**
   ```micropatterns
   VAR $variable_name [= expression]
   ```
   *   Declares a variable, requiring the `$` prefix. Variable names (after the `$`) are **case-insensitive**. Cannot conflict with environment variable names (case-insensitively).
   *   **Optional Initialization:** You can optionally assign an initial value using `= expression`.
       *   If `= expression` is omitted, the variable is initialized to `0`.
       *   If `= expression` is present, the variable is initialized to the result of the expression.
   *   **Expressions:** Limited to integer math: `value [+-*/%] value ...`
       *   `value` can be an integer literal, `$variable` (case-insensitive), or an environment variable (`$HOUR`, etc. - case-insensitive). Variables used in the expression must have been previously declared (either by `VAR` earlier in the script or standard environment variables).
       *   Operations are performed with standard precedence (`*`, `/`, `%` before `+`, `-`) and left-to-right associativity for equal precedence.
       *   Parentheses are **NOT** supported.
       *   Division `/` is integer division (truncates towards zero).
       *   Modulo `%` gives the remainder. Division/Modulo by zero is a runtime error.
   *   Example (declaration only): `VAR $offset` (initializes $offset to 0)
   *   Example (declaration and initialization): `VAR $angle = $minute * 6` (uses `$MINUTE`)
   *   Example (declaration and initialization): `VAR $offset = $counter % 20 + 10` (uses `$COUNTER`)

*   **Assignment (using LET):**
   ```micropatterns
   LET $variable_name = expression
   ```
   *   Assigns the result of an expression to a *previously declared* variable (using `VAR`), requiring the `$` prefix for the target variable (case-insensitive lookup of the name after `$`).
   *   Use `LET` to change the value of a variable after its initial declaration.
   *   Expression rules are the same as for `VAR` initialization.
   *   Example: `LET $offset = $offset + 1`

### Pattern Definition

*   **Patterns:**
   ```micropatterns
   DEFINE PATTERN NAME="pattern_name" WIDTH=w HEIGHT=h DATA="0110..."
   ```
   *   Defines a reusable monochrome bitmap pattern. Must appear before usage. This pattern can be used for filling areas (`FILL`) or drawn directly (`DRAW`).
   *   `NAME`: Identifier string (in double quotes). The identifier itself is **case-insensitive** for definition and lookup (e.g., `DEFINE PATTERN NAME="Checker"` is the same as `FILL NAME="checker"` or `DRAW NAME="checker"`).
   *   `WIDTH`, `HEIGHT`: Dimensions (integers, 1-20 recommended).
   *   `DATA`: String of '0' (white/transparent) and '1' (draw color) characters, row by row. Length must equal `WIDTH * HEIGHT`. If length mismatches, it will be padded with '0' or truncated with a warning.
   *   Maximum of 16 patterns can be defined per script.

### Drawing State Commands

*   **Color:**
   ```micropatterns
   COLOR NAME=BLACK
   COLOR NAME=WHITE
   ```
   *   Sets the current drawing color for subsequent primitives, fills, and drawn patterns. Default is `BLACK`. `NAME` value is case-insensitive.
   *   **Behavior with Patterns:**
       *   **When `COLOR NAME=BLACK` (Normal Mode):**
           *   **`DRAW NAME="pattern"`:**
               *   Pattern '1's (foreground pixels) are drawn `BLACK`.
               *   Pattern '0's (background pixels) are **transparent** (nothing is drawn, underlying content shows).
           *   **`FILL_RECT`, `FILL_CIRCLE`, `FILL_PIXEL` (with `FILL NAME="pattern"`):**
               *   Pattern '1's are drawn `BLACK`.
               *   Pattern '0's are drawn `WHITE` (filling the background of the shape with white where the pattern is '0').
       *   **When `COLOR NAME=WHITE` (Inverted/Special Mode):**
           *   **`DRAW NAME="pattern"`:**
               *   Pattern '1's are drawn `WHITE`.
               *   Pattern '0's remain **transparent**.
           *   **`FILL_RECT`, `FILL_CIRCLE`, `FILL_PIXEL` (with `FILL NAME="pattern"`):**
               *   Pattern '1's are drawn `WHITE`.
               *   Pattern '0's are drawn `BLACK` (fully inverting the pattern fill).

*   **Fill Pattern:**
   ```micropatterns
   FILL NAME="pattern_name"
   FILL NAME=SOLID
   ```
   *   Sets the fill pattern for `FILL_RECT` and `FILL_CIRCLE`.
   *   `NAME="pattern_name"`: Uses a previously defined pattern (identifier lookup is **case-insensitive**).
   *   `NAME=SOLID`: Resets to solid fill using the current `COLOR`. Default is `SOLID`. `SOLID` keyword is case-insensitive.

### Transformation Commands

*   Transformations apply cumulatively to subsequent drawing commands (`PIXEL`, `LINE`, `RECT`, `FILL_RECT`, `CIRCLE`, `FILL_CIRCLE`, `DRAW`).
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
   *   Applies an additional rotation of `d` degrees *cumulatively* to the current transformation state, around the current origin. `d` is an integer (0-359, wraps around). Uses integer math internally (e.g., precomputed sin/cos tables).

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
   *   Draws a filled rectangle. Fill uses the current fill pattern (set by `FILL`, tiled, case-insensitive lookup) or `COLOR` (if `FILL NAME=SOLID`). The fill area is determined by the transformed rectangle shape. (Note: Pattern fill on rotated rectangles may be approximated in some implementations).

*   **Circle Outline:**
   ```micropatterns
   CIRCLE X=x Y=y RADIUS=r
   ```
   *   Draws the outline of a circle centered at transformed `(x,y)` with radius `r` in the current `COLOR`. Uses Midpoint/Bresenham circle algorithm. The radius `r` is multiplied by the current `SCALE` factor.

*   **Filled Circle:**
   ```micropatterns
   FILL_CIRCLE X=x Y=y RADIUS=r
   ```
   *   Draws a filled circle. Fill uses the current fill pattern (set by `FILL`, case-insensitive lookup) or `COLOR`. The radius `r` is multiplied by the current `SCALE` factor.

*   **Draw Pattern:**
   ```micropatterns
   DRAW NAME="pattern_name" X=x Y=y
   ```
   *   Draws the previously defined pattern named `"pattern_name"` (identifier lookup is **case-insensitive**).
   *   The pattern's local top-left corner `(0,0)` is placed at the transformed `(x,y)`.
   *   The pattern's pixels are then individually transformed relative to this origin using the current `ROTATE` and `SCALE` state.
   *   Draws using the current `COLOR` for '1' pixels in the pattern data.

*   **Filled Pixel (Conditional):**
    ```micropatterns
    FILL_PIXEL X=x Y=y
    ```
    *   Draws a single pixel at the transformed `(x,y)`, but *only* if the current fill pattern allows it at that screen location.
    *   Uses the current fill pattern (set by `FILL`, tiled, case-insensitive lookup) or `COLOR` (if `FILL NAME=SOLID`).
    *   The pattern is checked at the resulting *screen* coordinate after transformation. If the pattern bit at that tiled location is '1' (or if fill is `SOLID`), the pixel is drawn using the current `COLOR`. Otherwise, nothing is drawn.

### Control Flow

*   **Repeat Loop:**
   ```micropatterns
   REPEAT COUNT=n
       # Commands to repeat...
       # $INDEX is available here (0 to n-1)
   ENDREPEAT
   ```
   *   Executes the block of commands `n` times. `n` can be an integer literal or variable (`$var` - case-insensitive), must be >= 0.
   *   The `COUNT` parameter specifies the number of iterations.
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

```DEFINE PATTERN NAME="zigzag" WIDTH=20 HEIGHT=20 DATA="0000000000011000000000000011000011000000100000011100011000001110000011100011000001110000011000000000011110001111000000000011100001111000000000001110001110000100000011110000110011000000001110001110000001100011100101110000001000011100001110000000000011100000100001000000011100000110011000000011100000110011000000001100000100011100000001100000000011100000000100000000011100000000000000000001100000000000"

VAR $center_x
VAR $center_y
VAR $secondplus = 30 + $SECOND
VAR $secondplusone = 1 + $SECOND
VAR $rotation
VAR $size
VAR $counterplusmod = 12 + $counter % $secondplusone

# fill background
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT WIDTH=$WIDTH HEIGHT=$HEIGHT X=0 Y=0

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $rotation = 720 * 89 / $secondplus
ROTATE DEGREES=$rotation

LET $size = 10 + $COUNTER % 10
VAR $halfsize = $size/2

FILL NAME="zigzag"
COLOR NAME=BLACK

REPEAT COUNT=$counterplusmod

 ROTATE DEGREES=$rotation
 VAR $shift = $MINUTE / $secondplusone
 TRANSLATE dx=$shift dy=$shift

 VAR $radius = $INDEX * 10 % 50
 VAR $Xposition= $INDEX
 VAR $Yposition= $INDEX

 IF $INDEX % 2 == 0 THEN
  COLOR NAME=WHITE
  SCALE FACTOR=$size
 ELSE
  COLOR NAME=BLACK
  SCALE FACTOR=$halfsize
 ENDIF
 
 FILL_RECT WIDTH=$INDEX HEIGHT=$INDEX X=$Xposition Y=$Yposition
 
 IF $INDEX % 3 == 0 THEN
  COLOR NAME=WHITE
  SCALE FACTOR=$size
 ELSE
  COLOR NAME=BLACK
  SCALE FACTOR=$halfsize
 ENDIF
 
 ROTATE DEGREES=$rotation
 FILL_RECT WIDTH=$INDEX HEIGHT=$INDEX X=$Xposition Y=$Yposition

 
 DRAW name="zigzag" x=$Xposition y=$Yposition
 
ENDREPEAT
```