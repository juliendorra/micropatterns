document.addEventListener('DOMContentLoaded', () => {
    const scriptInputTextArea = document.getElementById('scriptInput');
    const runButton = document.getElementById('runButton');
    const incrementCounterButton = document.getElementById('incrementCounterButton');
    const resetCounterButton = document.getElementById('resetCounterButton');
    const canvas = document.getElementById('displayCanvas');
    const ctx = canvas.getContext('2d');
    const errorLog = document.getElementById('errorLog');
    const assetPreviewsContainer = document.getElementById('assetPreviews');
    const realTimeHourSpan = document.getElementById('realTimeHourSpan');
    const realTimeMinuteSpan = document.getElementById('realTimeMinuteSpan');
    const realTimeSecondSpan = document.getElementById('realTimeSecondSpan');
    const lineWrapToggle = document.getElementById('lineWrapToggle'); // Get the checkbox

    // Script Management UI
    const scriptListSelect = document.getElementById('scriptList');
    const loadScriptButton = document.getElementById('loadScriptButton');
    const scriptNameInput = document.getElementById('scriptName');
    const saveScriptButton = document.getElementById('saveScriptButton');
    const newScriptButton = document.getElementById('newScriptButton');
    const scriptMgmtStatus = document.getElementById('scriptMgmtStatus');

    // --- Configuration ---
    // Assume server runs on localhost:8000 during development
    // TODO: Make this configurable or detect environment
    const API_BASE_URL = 'http://localhost:8000';
    // --- End Configuration ---


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
        lineWrapping: true, // Start with line wrapping enabled
        extraKeys: { "Tab": "autocomplete" },
        // Use the custom hint function for MicroPatterns specific suggestions
        hintOptions: { hint: micropatternsHint }
    });

    console.log("CodeMirror editor initialized:", codeMirrorEditor); // Check if editor object exists

    // --- Line Wrap Toggle Logic ---
    if (lineWrapToggle && codeMirrorEditor) {
        // Set initial state from checkbox (should be checked/true)
        codeMirrorEditor.setOption('lineWrapping', lineWrapToggle.checked);

        // Add listener to update editor when checkbox changes
        lineWrapToggle.addEventListener('change', () => {
            codeMirrorEditor.setOption('lineWrapping', lineWrapToggle.checked);
        });
    }
    // --- End Line Wrap Toggle Logic ---

    // --- Autocompletion Logic ---

    // Updated keywords for DEFINE PATTERN, FILL, DRAW
    const micropatternsKeywords = [
        // Commands
        "DEFINE", "PATTERN", "VAR", "LET", "COLOR", "FILL", "DRAW", "RESET_TRANSFORMS",
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
            // Special case: after DEFINE, suggest PATTERN
            if (token.string.toUpperCase() === 'DEFINE') {
                suggestions = ['PATTERN'];
            } else {
                suggestions = micropatternsKeywords.filter(k => k.includes("=")); // Suggest parameters
            }
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

    // --- Real-Time Display ---
    function updateRealTimeDisplay() {
        const now = new Date();
        const hours = String(now.getHours()).padStart(2, '0');
        const minutes = String(now.getMinutes()).padStart(2, '0');
        const seconds = String(now.getSeconds()).padStart(2, '0');

        // Update individual spans if they exist
        if (realTimeHourSpan) realTimeHourSpan.textContent = hours;
        if (realTimeMinuteSpan) realTimeMinuteSpan.textContent = minutes;
        if (realTimeSecondSpan) realTimeSecondSpan.textContent = seconds;
    }

    // Update time display immediately and then every second
    updateRealTimeDisplay();
    setInterval(updateRealTimeDisplay, 1000);
    // --- End Real-Time Display ---


    function getEnvironmentVariables() {
        const now = new Date();
        const currentHour = now.getHours();
        const currentMinute = now.getMinutes();
        const currentSecond = now.getSeconds();

        // Use override value if input is not empty, otherwise use real-time
        const hourOverride = env.HOUR.value.trim();
        const minuteOverride = env.MINUTE.value.trim();
        const secondOverride = env.SECOND.value.trim();

        const hour = hourOverride !== '' ? (parseInt(hourOverride, 10) || 0) : currentHour;
        const minute = minuteOverride !== '' ? (parseInt(minuteOverride, 10) || 0) : currentMinute;
        const second = secondOverride !== '' ? (parseInt(secondOverride, 10) || 0) : currentSecond;

        // Clamp values to valid ranges just in case
        const clamp = (val, min, max) => Math.max(min, Math.min(max, val));

        return {
            HOUR: clamp(hour, 0, 23),
            MINUTE: clamp(minute, 0, 59),
            SECOND: clamp(second, 0, 59),
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

    function runScript() {
        errorLog.textContent = ''; // Clear previous errors
        clearDisplay();

        const scriptText = codeMirrorEditor.getValue(); // Get text from CodeMirror
        const environment = getEnvironmentVariables();
        const parser = new MicroPatternsParser();
        let parseResult;
        let hasErrors = false;

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
                hasErrors = true;
                // Still update UI and render previews even with parse errors,
                // as some assets might have been defined before the error.
                renderAssetPreviews(parseResult.assets);
                return;
            }
            // Update UI only if parsing was successful enough to get assets
            // Render interactive previews
            renderAssetPreviews(parseResult.assets);
        } catch (e) {
            // Catch unexpected errors during the parse() call itself
            displayError(`Unexpected Parser Crash: ${e.message}`, "Fatal Parse");
            console.error(e);
            hasErrors = true;
            return;
        }


        // --- Runtime Phase ---
        const runtime = new MicroPatternsRuntime(
            ctx,
            parseResult.assets, // Pass the whole assets object { assets: {...} }
            environment,
            (runtimeErrorMessage) => {
                // Runtime errors should already be formatted with line numbers by runtimeError helper
                displayError(runtimeErrorMessage, "Runtime");
                hasErrors = true;
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
            hasErrors = true;
        }

        // Auto-increment counter if no errors occurred
        if (!hasErrors) {
            env.COUNTER.value = parseInt(env.COUNTER.value, 10) + 1;
        }
    }

    runButton.addEventListener('click', runScript);

    incrementCounterButton.addEventListener('click', () => {
        env.COUNTER.value = parseInt(env.COUNTER.value, 10) + 1;
        runScript(); // Re-run script after incrementing
    });

    resetCounterButton.addEventListener('click', () => {
        env.COUNTER.value = 0; // Reset counter to zero
        // Don't run script after resetting as per requirements
    });

    // --- Asset Preview Rendering and Editing ---

    const PREVIEW_SCALE = 8; // How many screen pixels per asset pixel

    // Updated to handle single assets.assets dictionary
    function renderAssetPreviews(assets) {
        assetPreviewsContainer.innerHTML = ''; // Clear previous previews

        const allAssets = Object.values(assets.assets || {});

        if (allAssets.length === 0) {
            assetPreviewsContainer.innerHTML = '<p style="color: #777; font-style: italic;">No patterns defined in the current script.</p>';
            return;
        }

        // Render all items defined via DEFINE PATTERN
        allAssets.forEach(asset => renderSingleAssetPreview(asset));
    }

    // Updated: assetType is no longer needed as a parameter, always 'PATTERN' conceptually
    function renderSingleAssetPreview(asset) {
        const container = document.createElement('div');
        container.className = 'asset-preview-item';

        const label = document.createElement('label');
        // Use the original case name if stored, otherwise use the uppercase key
        const displayName = asset.originalName || asset.name;
        // Label just shows PATTERN now
        label.textContent = `${displayName} (${asset.width}x${asset.height})`;
        container.appendChild(label);

        const canvas = document.createElement('canvas');
        const canvasWidth = asset.width * PREVIEW_SCALE;
        const canvasHeight = asset.height * PREVIEW_SCALE;
        canvas.width = canvasWidth;
        canvas.height = canvasHeight;
        canvas.style.width = `${canvasWidth}px`;
        canvas.style.height = `${canvasHeight}px`;

        const ctx = canvas.getContext('2d');
        drawAssetOnCanvas(ctx, asset, PREVIEW_SCALE);

        // Store asset info for click handler
        canvas.dataset.assetName = asset.name; // Use uppercase name for lookup
        // Store asset type as PATTERN since that's how it was defined
        canvas.dataset.assetType = 'PATTERN';

        canvas.addEventListener('click', (event) => {
            handlePreviewClick(event, canvas, asset); // Pass only asset now
        });

        container.appendChild(canvas);
        assetPreviewsContainer.appendChild(container);

        // --- START Drag and Drop Image Feature ---
        // Add drag and drop listeners to the canvas
        canvas.addEventListener('dragover', handleDragOver);
        canvas.addEventListener('dragleave', handleDragLeave);
        canvas.addEventListener('drop', (event) => handleDrop(event, canvas, asset));
        // --- END Drag and Drop Image Feature ---
    }


    // --- START Drag and Drop Image Feature ---

    function handleDragOver(event) {
        event.preventDefault(); // Necessary to allow dropping
        event.stopPropagation();
        // Add visual feedback class to the specific canvas being dragged over
        if (event.target.tagName === 'CANVAS') {
            event.target.classList.add('drop-target-active');
        }
        event.dataTransfer.dropEffect = 'copy'; // Show a copy icon
    }

    function handleDragLeave(event) {
        event.preventDefault();
        event.stopPropagation();
        // Remove visual feedback class from the specific canvas
        if (event.target.tagName === 'CANVAS') {
            event.target.classList.remove('drop-target-active');
        }
    }

    function handleDrop(event, targetCanvas, targetAsset) {
        event.preventDefault();
        event.stopPropagation();
        targetCanvas.classList.remove('drop-target-active'); // Remove feedback class

        console.log("Drop event on asset:", targetAsset.name);

        const files = event.dataTransfer.files;
        if (files.length !== 1) {
            displayError("Please drop exactly one image file.", "Drop Error");
            return;
        }

        const file = files[0];
        if (!file.type.startsWith('image/')) {
            displayError("Dropped file is not a recognized image type.", "Drop Error");
            return;
        }

        const reader = new FileReader();
        reader.onload = function(e) {
            const img = new Image();
            img.onload = function() {
                console.log(`Image loaded: ${img.width}x${img.height}. Resizing to pattern: ${targetAsset.width}x${targetAsset.height}`);

                // Create a temporary canvas to draw and resize the image
                const tempCanvas = document.createElement('canvas');
                tempCanvas.width = targetAsset.width;
                tempCanvas.height = targetAsset.height;
                const tempCtx = tempCanvas.getContext('2d');

                // Draw the image scaled down onto the temporary canvas
                // This performs the resize operation
                tempCtx.drawImage(img, 0, 0, targetAsset.width, targetAsset.height);

                // Get pixel data from the temporary (resized) canvas
                const imageData = tempCtx.getImageData(0, 0, targetAsset.width, targetAsset.height);
                const data = imageData.data; // RGBA array

                const newPixelData = [];
                for (let i = 0; i < data.length; i += 4) {
                    const r = data[i];
                    const g = data[i + 1];
                    const b = data[i + 2];
                    // Basic grayscale conversion (average)
                    const gray = (r + g + b) / 3;
                    // Apply 50% threshold (128)
                    // If average brightness is >= 128, pixel is white (0)
                    // If average brightness is < 128, pixel is black (1)
                    newPixelData.push(gray < 128 ? 1 : 0);
                }

                // Check if the generated data length matches
                if (newPixelData.length !== targetAsset.width * targetAsset.height) {
                     console.error(`Pixel data length mismatch after processing image. Expected ${targetAsset.width * targetAsset.height}, got ${newPixelData.length}`);
                     displayError(`Internal error processing image data for ${targetAsset.name}.`, "Drop Error");
                     return;
                }

                // Update the in-memory asset data
                targetAsset.data = newPixelData;

                // Redraw the preview canvas immediately
                const previewCtx = targetCanvas.getContext('2d');
                drawAssetOnCanvas(previewCtx, targetAsset, PREVIEW_SCALE);

                // Update the DATA string in the CodeMirror editor
                updateCodeMirrorAssetData(targetCanvas.dataset.assetType, targetAsset.name, targetAsset.data);

                console.log(`Pattern ${targetAsset.name} updated from dropped image.`);

            };
            img.onerror = function() {
                displayError(`Error loading dropped image for ${targetAsset.name}.`, "Drop Error");
            };
            img.src = e.target.result; // Set image source to the data URL
        };
        reader.onerror = function() {
            displayError(`Error reading dropped file for ${targetAsset.name}.`, "Drop Error");
        };
        reader.readAsDataURL(file); // Read the file as a data URL
    }

    // --- END Drag and Drop Image Feature ---


    function drawAssetOnCanvas(ctx, asset, scale) {
        const canvasWidth = asset.width * scale;
        const canvasHeight = asset.height * scale;
        ctx.clearRect(0, 0, canvasWidth, canvasHeight);
        ctx.fillStyle = 'white'; // Background for '0' pixels
        ctx.fillRect(0, 0, canvasWidth, canvasHeight);

        // Draw pixels
        for (let y = 0; y < asset.height; y++) {
            for (let x = 0; x < asset.width; x++) {
                const index = y * asset.width + x;
                if (asset.data[index] === 1) {
                    ctx.fillStyle = 'black';
                    ctx.fillRect(x * scale, y * scale, scale, scale);
                }
            }
        }

        // Draw grid lines
        ctx.strokeStyle = '#ddd'; // Light gray grid
        ctx.lineWidth = 1;
        for (let x = 0; x <= asset.width; x++) {
            ctx.beginPath();
            ctx.moveTo(x * scale, 0);
            ctx.lineTo(x * scale, canvasHeight);
            ctx.stroke();
        }
        for (let y = 0; y <= asset.height; y++) {
            ctx.beginPath();
            ctx.moveTo(0, y * scale);
            ctx.lineTo(canvasWidth, y * scale);
            ctx.stroke();
        }
    }

    // Updated: assetType is implicitly 'PATTERN' from dataset
    function handlePreviewClick(event, canvas, asset) {
        const rect = canvas.getBoundingClientRect();
        const clickX = event.clientX - rect.left;
        const clickY = event.clientY - rect.top;

        const assetX = Math.floor(clickX / PREVIEW_SCALE);
        const assetY = Math.floor(clickY / PREVIEW_SCALE);

        if (assetX >= 0 && assetX < asset.width && assetY >= 0 && assetY < asset.height) {
            const index = assetY * asset.width + assetX;
            // Toggle the pixel value in the in-memory asset object
            asset.data[index] = 1 - asset.data[index]; // Toggle 0 to 1 or 1 to 0

            // Redraw this specific canvas immediately
            const ctx = canvas.getContext('2d');
            drawAssetOnCanvas(ctx, asset, PREVIEW_SCALE);

            // Update the DATA string in the CodeMirror editor
            // Pass assetType from dataset ('PATTERN')
            updateCodeMirrorAssetData(canvas.dataset.assetType, asset.name, asset.data);
        }
    }

    // Updated: assetType parameter is used, regex looks for DEFINE PATTERN
    function updateCodeMirrorAssetData(assetType, assetNameUpper, newPixelData) {
        const editor = codeMirrorEditor; // Assuming codeMirrorEditor is accessible
        // Updated regex to find DEFINE PATTERN (assetType should always be 'PATTERN' here)
        const defineRegex = new RegExp(`^\\s*DEFINE\\s+${assetType}\\s+NAME\\s*=\\s*"([^"]+)"`, "i");
        const dataRegex = /DATA\s*=\s*"([01]*)"/i;

        let targetLine = -1;
        let lineContent = "";

        // Find the line number for the correct DEFINE PATTERN statement (case-insensitive name check)
        for (let i = 0; i < editor.lineCount(); i++) {
            const currentLine = editor.getLine(i);
            const match = currentLine.match(defineRegex);
            if (match && match[1].toUpperCase() === assetNameUpper) {
                targetLine = i;
                lineContent = currentLine;
                break;
            }
        }

        if (targetLine === -1) {
            console.error(`Could not find DEFINE ${assetType} NAME="${assetNameUpper}" line in editor.`);
            displayError(`Internal Error: Could not find DEFINE line for ${assetType} ${assetNameUpper} to update data.`, "Preview Edit");
            return;
        }

        // Find the DATA="..." part within that line
        const dataMatch = lineContent.match(dataRegex);
        if (!dataMatch) {
            console.error(`Could not find DATA="..." for ${assetType} ${assetNameUpper} on line ${targetLine + 1}.`);
            displayError(`Internal Error: Could not find DATA attribute for ${assetType} ${assetNameUpper} on line ${targetLine + 1}.`, "Preview Edit");
            return;
        }

        const newDataString = newPixelData.join('');
        const oldDataString = dataMatch[1];
        const dataStartIndex = lineContent.indexOf(dataMatch[0]) + dataMatch[0].indexOf('"') + 1; // Start after DATA="
        const dataEndIndex = dataStartIndex + oldDataString.length; // End before closing "

        const fromPos = CodeMirror.Pos(targetLine, dataStartIndex);
        const toPos = CodeMirror.Pos(targetLine, dataEndIndex);

        // Replace the data content in the editor
        editor.replaceRange(newDataString, fromPos, toPos, "+previewEdit"); // Use origin to avoid triggering unwanted events

        console.log(`Updated ${assetType} ${assetNameUpper} DATA on line ${targetLine + 1}`);
    }


    // --- End Asset Preview ---


    // --- End Asset Preview ---

    // --- Script Management Logic ---

    function resetEnvironmentInputs() {
        // Reset counter input to 0
        if (env.COUNTER) env.COUNTER.value = 0;
        // Clear override inputs
        if (env.HOUR) env.HOUR.value = '';
        if (env.MINUTE) env.MINUTE.value = '';
        if (env.SECOND) env.SECOND.value = '';
        console.log("Environment inputs (counter, overrides) reset.");
    }

    function setStatusMessage(message, isError = false) {
        if (scriptMgmtStatus) {
            scriptMgmtStatus.textContent = message;
            scriptMgmtStatus.style.color = isError ? 'red' : 'green';
            // Clear message after a delay
            setTimeout(() => {
                if (scriptMgmtStatus.textContent === message) { // Avoid clearing newer messages
                    scriptMgmtStatus.textContent = '';
                }
            }, isError ? 5000 : 3000);
        }
        if (isError) {
            console.error("Script Mgmt Error:", message);
        } else {
            console.log("Script Mgmt Status:", message);
        }
    }

    async function fetchScriptList() {
        console.log("Fetching script list...");
        setStatusMessage("Loading script list...");
        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts`);
            if (!response.ok) {
                throw new Error(`Failed to fetch script list: ${response.status} ${response.statusText}`);
            }
            const scripts = await response.json();

            // Clear existing options (except the default)
            scriptListSelect.options.length = 1;

            // Populate dropdown
            scripts.forEach(script => {
                const option = document.createElement('option');
                option.value = script.id;
                option.textContent = script.name;
                scriptListSelect.appendChild(option);
            });
            setStatusMessage("Script list loaded.", false);
            console.log("Script list loaded:", scripts);

        } catch (error) {
            setStatusMessage(`Error loading script list: ${error.message}`, true);
        }
    }

    async function loadScript(scriptId) {
        if (!scriptId) {
            setStatusMessage("Please select a script to load.", true);
            return;
        }
        console.log(`Loading script: ${scriptId}`);
        setStatusMessage(`Loading script '${scriptId}'...`);

        // Reset counter and overrides BEFORE loading
        resetEnvironmentInputs();

        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts/${scriptId}`);
            if (!response.ok) {
                 if (response.status === 404) {
                     throw new Error(`Script '${scriptId}' not found.`);
                 } else {
                    throw new Error(`Failed to load script: ${response.status} ${response.statusText}`);
                 }
            }
            const scriptData = await response.json();

            // Update UI
            scriptNameInput.value = scriptData.name || '';
            codeMirrorEditor.setValue(scriptData.content || '');
            setStatusMessage(`Script '${scriptData.name}' loaded successfully.`, false);
            console.log("Script loaded:", scriptData);

            // Optionally run the loaded script immediately
            runScript();

        } catch (error) {
            setStatusMessage(`Error loading script: ${error.message}`, true);
        }
    }

    async function saveScript() {
        const scriptName = scriptNameInput.value.trim();
        const scriptContent = codeMirrorEditor.getValue();

        if (!scriptName) {
            setStatusMessage("Please enter a name for the script before saving.", true);
            return;
        }

        // Generate a simple ID from the name (replace with more robust generation if needed)
        const scriptId = scriptName.toLowerCase()
                                 .replace(/\s+/g, '-') // Replace spaces with hyphens
                                 .replace(/[^a-z0-9-]/g, '') // Remove invalid characters
                                 .substring(0, 50); // Limit length

        if (!scriptId) {
             setStatusMessage("Invalid script name, cannot generate ID.", true);
             return;
        }

        console.log(`Saving script: ID=${scriptId}, Name=${scriptName}`);
        setStatusMessage(`Saving script '${scriptName}'...`);

        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts/${scriptId}`, {
                method: 'PUT',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    name: scriptName,
                    content: scriptContent,
                }),
            });

            if (!response.ok) {
                 const errorData = await response.json().catch(() => ({ error: 'Unknown error saving script' }));
                 throw new Error(`Failed to save script: ${response.status} ${response.statusText} - ${errorData.error || ''}`);
            }

            const result = await response.json();
            setStatusMessage(`Script '${result.script.name}' saved successfully!`, false);
            console.log("Script saved:", result);

            // Refresh the script list to include the new/updated script
            await fetchScriptList();
            // Select the saved script in the dropdown
            scriptListSelect.value = scriptId;

            // DO NOT re-run the script automatically after saving.
            // Keep the current counter and overrides.

        } catch (error) {
            setStatusMessage(`Error saving script: ${error.message}`, true);
        }
    }

    function newScript() {
        scriptNameInput.value = '';
        codeMirrorEditor.setValue(`# New MicroPatterns Script\n# Display is ${env.WIDTH}x${env.HEIGHT}\n\nCOLOR NAME=BLACK\nFILL NAME=SOLID\nFILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT\n\n`);
        scriptListSelect.value = ''; // Deselect any loaded script
        setStatusMessage("Cleared editor for new script.", false);
        runScript(); // Run the blank script template
    }

    // Add Event Listeners for Script Management
    if (loadScriptButton && scriptListSelect) {
        loadScriptButton.addEventListener('click', () => {
            loadScript(scriptListSelect.value);
        });
    }
    if (saveScriptButton && scriptNameInput) {
        saveScriptButton.addEventListener('click', saveScript);
    }
    if (newScriptButton) {
        newScriptButton.addEventListener('click', newScript);
    }

    // --- End Script Management Logic ---


    // Run once on load
    runScript();
    fetchScriptList(); // Fetch scripts when the page loads
});