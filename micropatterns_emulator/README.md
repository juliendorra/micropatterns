# MicroPatterns Emulator

This project provides a JavaScript-based emulator for the **MicroPatterns DSL**. It allows you to write, test, and visualize MicroPatterns scripts directly in your web browser.

For the full MicroPatterns language specification and project details, please see the main [README.md](../../README.md) at the root of the repository.

## Using the Emulator (`index.html`)

1.  **Open `index.html`:** Launch the file in a modern web browser.
2.  **Write Script:** Edit the MicroPatterns code in the text area on the left. A sample script is provided using the latest syntax (`DEFINE PATTERN`, `VAR $var`, `LET $var = ...`, `FILL`, `DRAW`).
3.  **Set Display Size:**
    *   Use the "Display Size" dropdown above the canvas to select dimensions.
    *   **540x960 (M5Paper)** is the default. It initially displays at 50% zoom (270x480 pixels on your screen) for better viewing.
    *   **200x200 (Default)** displays at 100% zoom.
    *   The `$WIDTH` and `$HEIGHT` environment variables available to your script will reflect the *actual selected resolution* (e.g., 540 and 960 for M5Paper).
4.  **Toggle Zoom (M5Paper):**
    *   If M5Paper (540x960) display is selected, use the "Zoom" button to toggle the visual display between 50% (default) and 100% (actual pixel size). This does not affect the script's `$WIDTH` or `$HEIGHT`.
5.  **Set Environment:** Adjust the `$HOUR`, `$MINUTE`, `$SECOND`, and `$COUNTER` values in the "Environment Variables" section.
6.  **User ID:**
    *   A unique User ID is automatically generated (using NanoID) and stored in your browser's local storage. This ID is displayed in the "Script Management" section.
    *   This User ID namespaces your scripts on the server. You can copy this ID to use in another browser to access the same set of scripts.
    *   You can also paste an existing User ID into the field.
7.  **Run Script:** Click the "Run Script" button. The script will be parsed and executed, drawing the result on the canvas display.
8.  **Increment Counter:** Click "Increment Counter" to increase the `$COUNTER` by one and automatically re-run the script, showing how the output changes over time (or iterations).
9.  **Errors:** Any parsing or runtime errors will appear in the red box below the script input, indicating the line number and error message.
10. **Patterns:** Patterns defined using `DEFINE PATTERN` in the script will be listed under "Patterns Defined" after a successful parse. Click on a preview to interactively edit its pixels in the editor. You can also drag & drop image files onto previews to import them.
11. **New Command:** Includes the `FILL_PIXEL` command for drawing pixels conditionally based on the current fill pattern.

## Emulator Implementation Notes

*   The emulator uses the HTML Canvas 2D API for rendering.
*   It simulates integer math where specified by the DSL (e.g., rotation, division in `LET`, coordinate calculations).
*   **Case-Insensitive:** Following the DSL spec, keywords, parameter names, variable names, pattern names, and environment variable names are treated case-insensitively by the parser and runtime. They are typically converted to uppercase internally for consistent handling.
*   Rotation uses pre-calculated sine/cosine tables scaled for integer operations, mimicking a potential target environment.
*   Pattern filling (`FILL`) for rotated rectangles is currently simplified/approximated and might not perfectly match a hardware implementation.
*   The parser (`parser.js`) and runtime (`runtime.js`) handle script execution and basic error checking/reporting.
*   Drawing primitives are implemented in `drawing.js`.
*   The main simulation loop and UI interaction are in `simulator.js`.