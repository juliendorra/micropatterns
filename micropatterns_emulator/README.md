# MicroPatterns Emulator

This project provides a JavaScript-based emulator for the **MicroPatterns DSL**. It allows you to write, test, and visualize MicroPatterns scripts directly in your web browser.

For the full MicroPatterns language specification and project details, please see the main [README.md](../README.md) at the root of the repository.

## Using the Emulator (`index.html`)

1.  **Open `index.html`:** Launch the file in a modern web browser.
2.  **Write Script:** Edit the MicroPatterns code in the text area on the left. A sample script is provided using the latest syntax (`DEFINE PATTERN`, `VAR $var`, `LET $var = ...`, `FILL`, `DRAW`).
3.  **Choose a device profile:**
    *   **M5Paper** uses Arduino-ESP32 2.0.4 and ESP-IDF 4.4.1 allocator behavior,
        with internal RAM and PSRAM. Its initial memory map is provisional until
        captured on hardware.
    *   **Watchy 2** uses Arduino-ESP32 3.1.3 and ESP-IDF 5.3.2 behavior. Its
        radios-off and BLE starting budgets are calibrated from the connected watch.
    *   **Unconstrained reference** uses the same C++ engine without device memory
        limits and remains the pixel-parity oracle.
    *   Select radios off, BLE, or Wi-Fi/TLS to include the corresponding memory
        reservation. The report says when the device would reboot and includes
        the failed allocation phase, source, request, free bytes, and largest block.
4.  **Set Display Size:**
    *   Use the "Display Size" dropdown above the canvas to select dimensions.
    *   **540x960 (M5Paper)** is the default. It initially displays at 50% zoom (270x480 pixels on your screen) for better viewing.
    *   **200x200 (Default)** displays at 100% zoom.
    *   The `$WIDTH` and `$HEIGHT` environment variables available to your script will reflect the *actual selected resolution* (e.g., 540 and 960 for M5Paper).
5.  **Toggle Zoom (M5Paper):**
    *   If M5Paper (540x960) display is selected, use the "Zoom" button to toggle the visual display between 50% (default) and 100% (actual pixel size). This does not affect the script's `$WIDTH` or `$HEIGHT`.
6.  **Set Environment:** Adjust the `$HOUR`, `$MINUTE`, `$SECOND`, and `$COUNTER` values in the "Environment Variables" section. These values can be manually typed into their respective input fields. The script will use these values when executed.
7.  **User ID:**
    *   A unique User ID is automatically generated (using NanoID) and stored in your browser's local storage. This ID is displayed in the "Script Management" section.
    *   This User ID namespaces your scripts on the server. You can copy this ID to use in another browser to access the same set of scripts.
    *   You can also paste an existing User ID into the field.
8.  **Run Script:** Click the "Run Script" button. The script will be parsed and executed, drawing the result on the canvas display. If the counter is unlocked (see below), it will auto-increment after a successful run.
9.  **Lock/Unlock Counter & Auto-Increment:**
    *   Next to the `$COUNTER` input, there is a lock button (showing 🔓 for unlocked, 🔒 for locked).
    *   Clicking this button toggles the lock state of the counter.
    *   **Unlocked State (🔓):** When the counter is unlocked, successfully running a script (either via "Run Script" or by other means that trigger a re-run) will automatically increment the `$COUNTER` value by one. This is useful for observing iterative changes.
    *   **Locked State (🔒):** When the counter is locked, it will **not** auto-increment after a script run. This is helpful for debugging or when you want to run the script multiple times with the same `$COUNTER` value. The background of the `$COUNTER` input field will be slightly grayed out to visually indicate it's locked.
    *   Manually changing the `$COUNTER` value in its input field is always possible, regardless of the lock state. The "Run Script" button will always use the currently displayed `$COUNTER` value.
10. **Errors:** Any parsing or runtime errors will appear in the red box below the script input, indicating the line number and error message.
11. **Patterns:** Patterns defined using `DEFINE PATTERN` in the script will be listed under "Patterns Defined" after a successful parse. Click on a preview to interactively edit its pixels in the editor. You can also drag & drop image files onto previews to import them.
    *   **Add a pattern:** Pick a size (8x8 up to 20x20, the parser's maximum) below the previews and click "Add new pattern". A new `DEFINE PATTERN` line is written into the script — after the last existing `DEFINE PATTERN`, or after the leading comment header if there is none, so it always precedes any `FILL`/`DRAW` that could use it. It starts as a checkerboard, visible as soon as you use it, and is then editable like any other preview.
    *   **Clone a pattern:** The duplicate icon next to a pattern's name copies its size and pixel data into a new `DEFINE PATTERN`. The copy is independent — editing it rewrites only its own line.
    *   **Naming:** Generated names never collide with a pattern already in the script. Existing names are read from the editor text rather than the last parse, so a script that currently fails to parse is still respected, and the comparison is case-insensitive to match the parser. New patterns are `pattern`, `pattern2`, …; clones are `foo-copy`, and cloning a clone gives `foo-copy2` rather than `foo-copy-copy`.
12. **New Command:** Includes the `FILL_PIXEL` command for drawing pixels conditionally based on the current fill pattern.

## Emulator Implementation Notes

*   Rendering is done by the **device firmware itself**, compiled to WebAssembly.
    The unconstrained `wasm/mp_render.{js,wasm}` is the pixel oracle.
    `mp_render_watchy` and `mp_render_m5paper` add version-matched Arduino
    `String`, ESP-IDF multi-heap/TLSF, heap poisoning, capability routing, and
    device object lifetimes. The same six C++ source files the M5Paper and
    the Watchy run produce the pixels here, byte-for-byte (verified against the
    firmware's golden images by `make verify-wasm` in `tools/host_harness`).
    Parse errors and pattern previews also come from the firmware's parser.
*   Run `make verify-wasm-constrained` in `tools/host_harness` for parity,
    OOM, recovery, state, persistence, and City 2 checks. Run `make sync-wasm`
    to rebuild and copy the canonical and constrained builds—all six static
    artifacts—into this directory. They are committed because this page deploys
    as static files with no build step.
*   `make ci` verifies parser-language contracts, renderer source parity, pixel
    goldens, memory safety, constrained behavior, and byte-for-byte artifact
    freshness. GitHub Actions also compiles both real firmwares, and deployment
    waits for those gates.
*   There used to be three JavaScript renderers here. They were removed once the
    WASM build existed; see
    [web-device-renderer-audit.md](../docs/analysis/web-device-renderer-audit.md).
*   The firmware parser supplies lint errors and pattern previews; there is no
    JavaScript renderer or second runtime. `simulator.js` owns only editor/UI
    behavior and the canvas transfer.
*   Resource simulation does not predict wall-clock time, watchdog expiry,
    FreeRTOS scheduling, radio timing, or e-ink waveforms. See
    [wasm-device-resource-fidelity.md](../docs/analysis/wasm-device-resource-fidelity.md)
    for the fidelity boundary.
