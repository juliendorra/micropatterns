#include <M5EPD.h>
#include "systeminit.h" // For M5Paper hardware init
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "micropatterns_drawing.h" // Drawing depends on runtime state
#include "global_setting.h"        // For GetTimeZone
#include "driver/rtc_io.h"         // For rtc_gpio functions and deep sleep configuration
#include "driver/gpio.h"           // For gpio_num_t

// --- Sample MicroPatterns Script ---
// Updated Savanna Example with IF/ELSE and working REPEAT/LET
const char *savanna_script = R"(
DEFINE PATTERN NAME="girafe" WIDTH=20 HEIGHT=20 DATA="1100000000000000000010001110000111000100000111111000110011100011111100000001111000111110001100111110000011100111100111000010010011111100000001110001111111000010111110011111100011111111000011111010111111110110011100100111011001100010011000010000111100001110000000011111100000100100001111111011000001100111111100111100111001111110011110011110111111001111001111100111100011100111110000100000010000111000"

DEFINE PATTERN NAME="background" WIDTH=10 HEIGHT=10 DATA="0010001001000101000010001000000101000000001000000001000000011000000011000001111000001000001000000000"

VAR $center_x
VAR $center_y
VAR $secondplusone
VAR $rotation
VAR $rotationdeux
VAR $size
var $squareside

LET $size = 10 + $COUNTER % 15
LET $secondplusone = 1 + $SECOND
LET $rotation = 360 * 60 / $secondplusone
LET $rotationdeux = $minute + 360 * 60 / $secondplusone 
LET $squareside = 20

LET $center_x = $width/2 
LET $center_y = $height/2

FILL NAME="background"
FILL_RECT WIDTH=$width HEIGHT=$height X=0 Y=0

TRANSLATE DX=$center_x DY=$center_y

SCALE FACTOR=$size

ROTATE DEGREES=$rotation

COLOR NAME=WHITE
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotationdeux

COLOR NAME=BLACK
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotation

COLOR NAME=WHITE
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotationdeux

COLOR NAME=BLACK
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0            
)";

// Global objects
M5EPD_Canvas canvas(&M5.EPD);
MicroPatternsParser parser;
MicroPatternsRuntime *runtime = nullptr; // Initialize later after canvas setup
RTC_DATA_ATTR int counter = 0;           // Use RTC memory to persist counter across deep sleep
RTC_Time time_struct;                    // To store time from RTC

void setup()
{
    pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
    M5.enableMainPower();

    // Initialize M5Paper hardware
    SysInit_Start(); // This function handles M5.begin(), EPD init, etc.

    log_i("MicroPatterns M5Paper v1.2 (Sleep Cycle %d)", counter);

    // Create canvas AFTER M5.EPD.begin() inside SysInit_Start()
    // Use hardcoded dimensions 540x960 for M5Paper
    canvas.createCanvas(540, 960);
    if (!canvas.frameBuffer())
    {
        log_e("Failed to create canvas framebuffer!");
        while (1)
            delay(1000); // Halt
    }
    log_i("Canvas created: %d x %d", canvas.width(), canvas.height());

    M5.EPD.Clear(true);

    // Parse the script
    log_i("Parsing script...");
    if (!parser.parse(savanna_script))
    { // Check return value
        log_e("Script parsing failed:");
        const auto &errors = parser.getErrors();
        for (const String &err : errors)
        {
            // Error messages from parser already include line number info
            log_e("  %s", err.c_str());
        }
        // Halt or indicate error on screen

        canvas.fillCanvas(0); // White
        canvas.setTextSize(2);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(15); // Black

        const int x_pos = 20;
        int y_pos = 50;
        canvas.drawString("Script Parse Error!", x_pos, y_pos);

        y_pos += 50;

        for (const String &err : errors)
        {
            // Draw each error message with an incremented y position
            canvas.drawString(err.c_str(), x_pos, y_pos);
            y_pos += 50; // Increment y position for the next error message
        }
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        while (1)
            delay(1000); // Halt
    }
    else
    {
        log_i("Script parsed successfully.");
        const auto &commands = parser.getCommands();
        log_i("Found %d top-level commands.", commands.size());
        const auto &assets = parser.getAssets();
        log_i("Found %d assets.", assets.size());
        const auto &vars = parser.getDeclaredVariables();
        log_i("Found %d declared variables.", vars.size());

        // Initialize runtime AFTER canvas is created and script is parsed successfully
        runtime = new MicroPatternsRuntime(&canvas, parser.getAssets());
        runtime->setCommands(&commands);
        runtime->setDeclaredVariables(&vars); // Pass declared variables to runtime
    }
}

void loop()
{
    // Execute the script once if runtime is valid
    if (runtime)
    {
        // Environment variables
        M5.RTC.getTime(&time_struct);
        counter++;

        // Update runtime environment
        log_d("Executing script - Counter: %d", counter);
        log_d("Executing script - Time: %d : %d : %d", time_struct.hour, time_struct.min, time_struct.sec);

        // Passing the environment variables to the runtime
        runtime->setTime(time_struct.hour, time_struct.min, time_struct.sec);
        runtime->setCounter(counter);

        // Execute the script
        runtime->execute(); // Executes commands and pushes canvas

        log_i("Executed script for cycle #%d (Time: %02d:%02d:%02d)", counter, time_struct.hour, time_struct.min, time_struct.sec);
    }
    else
    {
        log_e("Runtime not initialized, skipping execution.");
        // If runtime failed init (due to parse error), we are halted anyway.
        // If it failed for other reasons, log and prepare for sleep.
    }

    Shutdown();
}