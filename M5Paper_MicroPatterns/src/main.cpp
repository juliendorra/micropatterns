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

DEFINE PATTERN NAME="artdeco" WIDTH=20 HEIGHT=20 DATA="1011001110111100110110100110001001110111101010000000000110111101000110001000111011100001100111000100100010011001100110110101110011011011110100011110110100111000100001101101011100011110001101010110011011111011011111001110000011010110101100000000011011101110000001110001111101000110011000010111100001100110011010100011001101001111111101111011110111000010001110111101100011000001110110110011101111001101"


VAR $center_x
VAR $center_y
VAR $secondplusone
VAR $rotation
VAR $size

COLOR NAME=BLACK
FILL NAME="artdeco"

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $secondplusone = 1 + $SECOND
LET $rotation = 350 * 59 / $secondplusone
ROTATE DEGREES=$rotation

LET $size = 1 + $COUNTER % 20
SCALE FACTOR=$size

FILL_CIRCLE RADIUS=20 X=0 Y=0
              
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