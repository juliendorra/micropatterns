# MicroPatterns Emulator

This project provides a JavaScript-based emulator for the **MicroPatterns DSL**. It allows you to write, test, and visualize MicroPatterns scripts directly in your web browser.

For the full MicroPatterns language specification and project details, please see the main [README.md](../../README.md) at the root of the repository.

## Using the Emulator (`index.html`)

1.  **Open `index.html`:** Launch the file in a modern web browser.
2.  **Write Script:** Edit the MicroPatterns code in the text area on the left. A sample script is provided using the latest syntax (`DEFINE PATTERN`, `FILL`, `DRAW`).
3.  **Set Environment:** Adjust the `$HOUR`, `$MINUTE`, `$SECOND`, and `$COUNTER` values in the "Environment Variables" section. The display `$WIDTH` and `$HEIGHT` are fixed at 200x200 for this emulator.
4.  **Run Script:** Click the "Run Script" button. The script will be parsed and executed, drawing the result on the canvas display.
5.  **Increment Counter:** Click "Increment Counter" to increase the `$COUNTER` by one and automatically re-run the script, showing how the output changes over time (or iterations).
6.  **Errors:** Any parsing or runtime errors will appear in the red box below the script input, indicating the line number and error message.
7.  **Patterns:** Patterns defined using `DEFINE PATTERN` in the script will be listed under "Patterns Defined" after a successful parse. Click on a preview to interactively edit its pixels in the editor. You can also drag & drop image files onto previews to import them.
8.  **New Command:** Includes the `FILL_PIXEL` command for drawing pixels conditionally based on the current fill pattern.

## Emulator Implementation Notes

*   The emulator uses the HTML Canvas 2D API for rendering.
*   It simulates integer math where specified by the DSL (e.g., rotation, division in `LET`, coordinate calculations).
*   **Case-Insensitive:** Following the DSL spec, keywords, parameter names, variable names, pattern names, and environment variable names are treated case-insensitively by the parser and runtime. They are typically converted to uppercase internally for consistent handling.
*   Rotation uses pre-calculated sine/cosine tables scaled for integer operations, mimicking a potential target environment.
*   Pattern filling (`FILL`) for rotated rectangles is currently simplified/approximated and might not perfectly match a hardware implementation.
*   The parser (`parser.js`) and runtime (`runtime.js`) handle script execution and basic error checking/reporting.
*   Drawing primitives are implemented in `drawing.js`.
*   The main simulation loop and UI interaction are in `simulator.js`.