#include <M5EPD.h>
#include "systeminit.h" // For M5Paper hardware init
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "micropatterns_drawing.h" // Drawing depends on runtime state

// --- Sample MicroPatterns Script ---
// Savanna Example (Simplified for basic parser/runtime)
const char *savanna_script = R"(
# Savanna Example

DEFINE PATTERN NAME="noise" WIDTH=8 HEIGHT=8 DATA="1011010101001101110100100011010110101011011100101100101001011100"
DEFINE PATTERN NAME="grass" WIDTH=4 HEIGHT=4 DATA="0100101001000100"

VAR $ground_level
VAR $sky_level
VAR $sun_x
VAR $sun_y
VAR $tree_x
VAR $tree_y
VAR $tree_scale

LET $ground_level = $HEIGHT / 3 * 2
LET $sky_level = $HEIGHT / 3

# Sky (Solid White)
COLOR NAME=WHITE
FILL NAME=SOLID
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$sky_level

# Sun (Solid Black Circle)
COLOR NAME=BLACK
FILL NAME=SOLID
# Simplified position
LET $sun_x = $WIDTH / 4 
# Simplified position
LET $sun_y = $HEIGHT / 5 
FILL_CIRCLE X=$sun_x Y=$sun_y RADIUS=20

# Ground (Noise Pattern Fill)
COLOR NAME=BLACK
FILL NAME="noise"
FILL_RECT X=0 Y=$sky_level WIDTH=$WIDTH HEIGHT=$ground_level

# Grass (Draw Pattern Repeatedly)
# Grass color
COLOR NAME=BLACK 
 # Re-use variable
LET $tree_x = 0
REPEAT COUNT=5 TIMES 
    # Manual loop simulation: Draw grass pattern at various X positions
    # Need LET implemented properly for this to vary X
    # Would be ideal
    LET $tree_x = $INDEX * 10 
    # Draw at ground level
    # DRAW NAME="grass" X=$tree_x Y=$ground_level
ENDREPEAT
# Manual grass placement for now:
DRAW NAME="grass" X=30 Y=$ground_level
DRAW NAME="grass" X=80 Y=$ground_level
DRAW NAME="grass" X=130 Y=$ground_level
DRAW NAME="grass" X=180 Y=$ground_level
DRAW NAME="grass" X=230 Y=$ground_level
DRAW NAME="grass" X=280 Y=$ground_level
DRAW NAME="grass" X=330 Y=$ground_level
DRAW NAME="grass" X=380 Y=$ground_level
DRAW NAME="grass" X=430 Y=$ground_level
DRAW NAME="grass" X=480 Y=$ground_level

# Tree (Placeholder - requires more complex drawing/transforms)
# COLOR NAME=BLACK
# FILL NAME=SOLID
# LET $tree_x = $WIDTH / 2
# LET $tree_y = $ground_level - 50 # Trunk base
# LET $tree_scale = 2
# TRANSLATE DX=$tree_x DY=$tree_y
# SCALE FACTOR=$tree_scale
# Trunk
# FILL_RECT X=-5 Y=0 WIDTH=10 HEIGHT=50
# Leaves (Circle)
# FILL_CIRCLE X=0 Y=-75 RADIUS=30
)";

// Global objects
M5EPD_Canvas canvas(&M5.EPD);
MicroPatternsParser parser;
MicroPatternsRuntime *runtime = nullptr; // Initialize later after canvas setup
int counter = 0;

void setup()
{
    // Initialize M5Paper hardware
    SysInit_Start(); // This function handles M5.begin(), EPD init, etc.

    log_i("MicroPatterns M5Paper Firmware");

    // Create canvas AFTER M5.EPD.begin() inside SysInit_Start()
    // Use hardcoded dimensions 540x960 for M5Paper
    canvas.createCanvas(540, 960);
    if (!canvas.frameBuffer())
    {
        log_e("Failed to create canvas framebuffer!");
        while (1)
            ; // Halt
    }
    log_i("Canvas created: %d x %d", canvas.width(), canvas.height());

    // Parse the script
    log_i("Parsing script...");
    parser.parse(savanna_script); // Parse the script text

    const auto &errors = parser.getErrors();
    if (!errors.empty())
    {
        log_e("Script parsing failed:");
        for (const String &err : errors)
        {
            // Error messages from parser already include line number info
            log_e("  %s", err.c_str());
        }
        // Halt or indicate error on screen
        canvas.fillCanvas(0); // White
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(15); // Black
        canvas.drawString("Script Parse Error!", 10, 10);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        while (1)
            ; // Halt
    }
    else
    {
        log_i("Script parsed successfully.");
        const auto &commands = parser.getCommands();
        log_i("Found %d commands.", commands.size());
        const auto &assets = parser.getAssets();
        log_i("Found %d assets.", assets.size());
        const auto &vars = parser.getDeclaredVariables();
        log_i("Found %d declared variables.", vars.size());

        // Initialize runtime AFTER canvas is created and script is parsed successfully
        runtime = new MicroPatternsRuntime(&canvas, parser.getAssets());
        runtime->setCommands(&commands);      // Give runtime access to commands
        runtime->setDeclaredVariables(&vars); // Give runtime access to declared vars
    }

    // Initial render only if runtime was successfully initialized
    if (runtime)
    {
        log_i("Initial execution...");
        runtime->setCounter(counter);
        runtime->execute(); // This executes commands and pushes the canvas with GLD16
        log_i("Initial execution complete.");
    }
    else
    {
        log_e("Runtime not initialized, skipping initial execution.");
        // Keep the parse error message displayed if parsing failed
    }
}

void loop()
{
    // Update counter (or other dynamic inputs)
    counter++;

    // Re-execute the script if runtime is valid
    if (runtime)
    {
        runtime->setCounter(counter);
        runtime->execute(); // Executes commands and pushes canvas
    }
    else
    {
        log_e("Runtime not initialized, skipping execution.");
    }

    // Delay or wait for trigger
    delay(5000); // Update every 5 seconds for example
}