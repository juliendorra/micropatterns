document.addEventListener('DOMContentLoaded', () => {
    const scriptInputTextArea = document.getElementById('scriptInput');
    const runButton = document.getElementById('runButton');
    const incrementCounterButton = document.getElementById('incrementCounterButton');
    const canvas = document.getElementById('displayCanvas');
    const ctx = canvas.getContext('2d');
    const errorLog = document.getElementById('errorLog');
    const definedPatternsDiv = document.getElementById('definedPatterns');
    const definedIconsDiv = document.getElementById('definedIcons');

    const env = {
        HOUR: document.getElementById('envHour'),
        MINUTE: document.getElementById('envMinute'),
        SECOND: document.getElementById('envSecond'),
        COUNTER: document.getElementById('envCounter'),
        WIDTH: canvas.width,
        HEIGHT: canvas.height,
    };

    // Initialize CodeMirror
    const codeMirrorEditor = CodeMirror.fromTextArea(scriptInputTextArea, {
        lineNumbers: true,
        mode: "micropatterns", // Use the custom MicroPatterns mode
        theme: "neat", // Use a theme
        indentUnit: 4,
        tabSize: 4,
        lineWrapping: true,
    });

    function getEnvironmentVariables() {
        return {
            HOUR: parseInt(env.HOUR.value, 10) || 0,
            MINUTE: parseInt(env.MINUTE.value, 10) || 0,
            SECOND: parseInt(env.SECOND.value, 10) || 0,
            COUNTER: parseInt(env.COUNTER.value, 10) || 0,
            WIDTH: env.WIDTH,
            HEIGHT: env.HEIGHT,
        };
    }

    function displayError(message, type = "Error") {
        console.error(`${type}: ${message}`);
        // Prepend error type for clarity in the log
        errorLog.textContent += `${type}: ${message}\n`;
    }

    function clearDisplay() {
        ctx.fillStyle = 'white';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
    }

    function updateDefinedAssetsUI(assets) {
        const patternNames = Object.keys(assets.patterns);
        const iconNames = Object.keys(assets.icons);
        definedPatternsDiv.textContent = `Patterns: ${patternNames.length > 0 ? patternNames.join(', ') : 'None'}`;
        definedIconsDiv.textContent = `Icons: ${iconNames.length > 0 ? iconNames.join(', ') : 'None'}`;
    }

    function runScript() {
        errorLog.textContent = ''; // Clear previous errors
        clearDisplay();
        updateDefinedAssetsUI({ patterns: {}, icons: {} }); // Clear assets UI initially

        const scriptText = codeMirrorEditor.getValue(); // Get text from CodeMirror
        const environment = getEnvironmentVariables();
        const parser = new MicroPatternsParser();
        let parseResult;

        // --- Parsing Phase ---
        try {
            parseResult = parser.parse(scriptText);
            // Display parse errors, if any
            if (parseResult.errors.length > 0) {
                parseResult.errors.forEach(err => {
                    // Error message from ParseError already includes line number and type
                    displayError(err.message, "Parse");
                });
                // Stop execution if parsing failed critically
                // Allow continuing if only minor parse errors occurred? For now, stop.
                return;
            }
            // Update UI only if parsing was successful enough to get assets
            updateDefinedAssetsUI(parseResult.assets);
        } catch (e) {
            // Catch unexpected errors during the parse() call itself
            displayError(`Unexpected Parser Crash: ${e.message}`, "Fatal Parse");
            console.error(e);
            return;
        }


        // --- Runtime Phase ---
        const runtime = new MicroPatternsRuntime(
            ctx,
            parseResult.assets,
            environment,
            (runtimeErrorMessage) => {
                // Runtime errors should already be formatted with line numbers by runtimeError helper
                 displayError(runtimeErrorMessage, "Runtime");
            }
        );

        try {
            runtime.execute(parseResult.commands);
        } catch (e) {
            // Catch errors that might escape the runtime's internal try-catch
            // (e.g., issues within the error callback itself)
            // The runtime's error callback should have already displayed the formatted error.
            // We log it here just in case.
            console.error("Unhandled Runtime Exception:", e);
            if (!e.isRuntimeError) { // Display if not already handled by callback
                 displayError(`Unexpected Runtime Crash: ${e.message}`, "Fatal Runtime");
            }
        }
    }

    runButton.addEventListener('click', runScript);

    incrementCounterButton.addEventListener('click', () => {
        env.COUNTER.value = parseInt(env.COUNTER.value, 10) + 1;
        runScript(); // Re-run script after incrementing
    });

    // Run once on load
    runScript();
});