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
        // Change key binding to just "Alt". Note: This might have side effects or conflicts.
        // A combination like "Alt-Q" might be more reliable if issues arise.
        extraKeys: { "Tab": "autocomplete" },
        // Use the custom hint function for MicroPatterns specific suggestions
        hintOptions: { hint: micropatternsHint }
    });

    console.log("CodeMirror editor initialized:", codeMirrorEditor); // Check if editor object exists

    // --- Autocompletion Logic ---

    const micropatternsKeywords = [
        // Commands
        "DEFINE", "PATTERN", "ICON", "VAR", "LET", "COLOR", "RESET_TRANSFORMS",
        "TRANSLATE", "ROTATE", "SCALE", "PIXEL", "LINE", "RECT", "FILL_RECT",
        "CIRCLE", "FILL_CIRCLE", "REPEAT", "TIMES", "IF", "THEN", "ELSE",
        "ENDIF", "ENDREPEAT",
        // Parameters (often followed by =)
        "NAME=", "WIDTH=", "HEIGHT=", "DATA=", "X=", "Y=", "X1=", "Y1=", "X2=", "Y2=",
        "DX=", "DY=", "DEGREES=", "FACTOR=", "RADIUS=", "COUNT=",
        // Specific Values
        "BLACK", "WHITE", "SOLID"
    ];
    const micropatternsEnvVars = [
        "$HOUR", "$MINUTE", "$SECOND", "$COUNTER", "$WIDTH", "$HEIGHT", "$INDEX"
    ];

    function getUserDefinedVars(editor) {
        const text = editor.getValue();
        const vars = new Set();
        // Simple regex to find VAR declarations (case-insensitive)
        const varRegex = /^VAR\s+([a-zA-Z_][a-zA-Z0-9_]*)/gmi;
        let match;
        while ((match = varRegex.exec(text)) !== null) {
            // Store with leading $ and uppercase for consistency
            vars.add("$" + match[1].toUpperCase());
        }
        return Array.from(vars);
    }

    function micropatternsHint(editor) {
        console.log("--- micropatternsHint triggered ---"); // Log trigger
        const cursor = editor.getCursor();
        const token = editor.getTokenAt(cursor);
        const line = editor.getLine(cursor.line);
        console.log("Cursor:", cursor, "Token:", token, "Line:", line); // Log basic info
        const start = token.start;
        const end = cursor.ch; // Use cursor position for end, token.end might be too far
        const currentWord = token.string.substring(0, end - start).toUpperCase(); // Get word being typed, uppercase
        console.log("Current Word:", currentWord, "Token Start:", start, "Cursor End:", end); // Log word calculation

        let suggestions = [];
        const userVars = getUserDefinedVars(editor);
        const allVars = [...micropatternsEnvVars, ...userVars];
        console.log("User Vars:", userVars, "All Vars:", allVars); // Log variables found

        // Basic context detection (can be improved)
        const isStartOfLine = token.start === 0 && line.trim().toUpperCase().startsWith(currentWord);
        const isAfterEquals = line.substring(0, start).includes("=");
        // Refined value check: after equals OR if the token itself is a variable/number/operator (suggesting continuation)
        // OR if the token is just starting ($)
        const isPossiblyValue = isAfterEquals || ['variable-2', 'variable-3', 'number', 'operator'].includes(token.type) || token.string === '$';
        console.log("Context:", { isStartOfLine, isAfterEquals, isPossiblyValue }); // Log context flags

        // Suggest Commands at start of line
        if (isStartOfLine) {
            console.log("Context: Start of line");
            suggestions = micropatternsKeywords.filter(k => !k.includes("=")); // Only suggest commands
        }
        // Suggest Parameters after a command word (simple check)
        else if (token.type === 'keyword' && !micropatternsKeywords.includes(token.string.toUpperCase() + "=")) {
            suggestions = micropatternsKeywords.filter(k => k.includes("=")); // Suggest parameters
        }
        // Suggest Variables and specific values if expecting a value
        else if (isPossiblyValue || isAfterEquals) {
            suggestions = [
                ...allVars,
                "BLACK", "WHITE", "SOLID" // Suggest keywords usable as values
            ];
        }
        // Default: suggest everything? Or refine context detection
        else {
            console.log("Context: Default/Unknown");
            suggestions = [
                ...micropatternsKeywords,
                ...allVars
            ];
        }
        console.log("Initial Suggestions:", suggestions); // Log suggestions before filtering


        // Filter suggestions based on the current word being typed
        const filteredSuggestions = suggestions.filter(item =>
            item.toUpperCase().startsWith(currentWord)
        );

        // If the current word exactly matches a suggestion, don't show the hint list
        // unless there are other options starting with the same prefix.
        if (filteredSuggestions.length === 1 && filteredSuggestions[0].toUpperCase() === currentWord) {
            // Check if the exact match is the *only* possibility
            const moreOptionsExist = suggestions.some(s => s.toUpperCase().startsWith(currentWord) && s.toUpperCase() !== currentWord);
            if (!moreOptionsExist) {
                return null; // Don't show hint if it's an exact and only match
            }
        }


        if (filteredSuggestions.length > 0) {
            const hintObject = {
                list: filteredSuggestions,
                from: CodeMirror.Pos(cursor.line, start),
                to: CodeMirror.Pos(cursor.line, end)
            };
            console.log("Filtered Suggestions:", filteredSuggestions); // Log the filtered list
            console.log("Returning Hint Object:", hintObject); // Log the final object
            return hintObject;
        }
        console.log("No suggestions match."); // Log if filtering removed everything
        return null; // No suggestions
    }
    // --- End Autocompletion Logic ---


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