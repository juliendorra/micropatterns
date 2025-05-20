import { MicroPatternsCompiler } from './compiler.js';
import { MicroPatternsCompiledRunner } from './compiled_runtime.js';
import { MicroPatternsParser } from './parser.js';
import { MicroPatternsRuntime } from './runtime.js';
import { DisplayListGenerator } from './display_list_generator.js';
import { DisplayListRenderer } from './display_list_renderer.js';

document.addEventListener('DOMContentLoaded', async () => {

    let executionPath = 'displayList'; // 'interpreter', 'compiler', or 'displayList'
    // let USE_COMPILER = true; // This will be replaced by executionPath logic
    let currentRuntimeInstance = null; // Store runtime instance for profiling access
    let currentCompilerInstance = null; // For compiler instance access
    let currentCompiledRunnerInstance = null; // For compiled runner instance access
    let currentDisplayListGenerator = null; // For display list generator instance
    let currentDisplayListRenderer = null; // For display list renderer instance

    let profilingEnabled = false; // Initialize profiling flag
    let profilingData = {}; // Storage for profiling metrics
    
    let isCounterLocked = false; // State for counter lock
    
    // SVG Icons are now directly in HTML. CSS will handle visibility.

    const scriptInputTextArea = document.getElementById('scriptInput');
    const runButton = document.getElementById('runButton');
    const lockCounterButton = document.getElementById('lockCounterButton'); // Changed ID
    const resetCounterButton = document.getElementById('resetCounterButton');
    const canvas = document.getElementById('displayCanvas');
    const ctx = canvas.getContext('2d');
    const errorLog = document.getElementById('errorLog');
    const assetPreviewsContainer = document.getElementById('assetPreviews');
    const realTimeHourSpan = document.getElementById('realTimeHourSpan');
    const realTimeMinuteSpan = document.getElementById('realTimeMinuteSpan');
    const realTimeSecondSpan = document.getElementById('realTimeSecondSpan');
    const lineWrapToggle = document.getElementById('lineWrapToggle'); // Get the checkbox

    // Display Size UI
    const displaySizeSelect = document.getElementById('displaySizeSelect');
    const displayInfoSpan = document.getElementById('displayInfoSpan');
    const zoomToggleButton = document.getElementById('zoomToggleButton');

    // Theme Switcher UI
    const themeStylesheet = document.getElementById('themeStylesheet');
    const themeSelect = document.getElementById('themeSelect');

    // Optimization UI
    // const optimizationContainer = document.getElementById('optimizationSettings') ||
    //                            document.createElement('div'); // This is not strictly needed as index.html is the source of truth for the structure

    // --- End Configuration ---

    // Configuration for UI checkboxes related to optimizations
    const checkboxesConfig = [
        // Common / Interpreter / Compiler
        { id: 'enableOverdrawOptimization', configKey: 'enableOverdrawOptimization', label: 'Overdraw optimization (Pixel Occupancy)' },
        { id: 'enableTransformCaching', configKey: 'enableTransformCaching', label: 'Transform caching' },
        { id: 'enablePatternTileCaching', configKey: 'enablePatternTileCaching', label: 'Pattern tile caching' },
        { id: 'enablePixelBatching', configKey: 'enablePixelBatching', label: 'Pixel batching' },
        // Compiler specific
        { id: 'enableLoopUnrolling', configKey: 'enableLoopUnrolling', label: 'Loop unrolling', compilerOnly: true },
        { id: 'enableInvariantHoisting', configKey: 'enableInvariantHoisting', label: 'Invariant hoisting', compilerOnly: true },
        { id: 'enableFastPathSelection', configKey: 'enableFastPathSelection', label: 'Fast path selection', compilerOnly: true },
        { id: 'enableSecondPassOptimization', configKey: 'enableSecondPassOptimization', label: 'Second-pass optimization', compilerOnly: true },
        { id: 'enableDrawCallBatching', configKey: 'enableDrawCallBatching', label: 'Draw call batching', compilerOnly: true, secondPassDependent: true },
        { id: 'enableDeadCodeElimination', configKey: 'enableDeadCodeElimination', label: 'Dead code elimination', compilerOnly: true, secondPassDependent: true },
        { id: 'enableConstantFolding', configKey: 'enableConstantFolding', label: 'Constant folding', compilerOnly: true, secondPassDependent: true },
        { id: 'enableTransformSequencing', configKey: 'enableTransformSequencing', label: 'Transform sequencing', compilerOnly: true, secondPassDependent: true },
        { id: 'enableDrawOrderOptimization', configKey: 'enableDrawOrderOptimization', label: 'Draw order optimization', compilerOnly: true, secondPassDependent: true },
        { id: 'enableMemoryOptimization', configKey: 'enableMemoryOptimization', label: 'Memory optimization', compilerOnly: true, secondPassDependent: true },
        // Logging
        { id: 'logOptimizationStats', configKey: 'logOptimizationStats', label: 'Log optimization stats' },
        { id: 'logProfilingReport', configKey: 'logProfilingReport', label: 'Log profiling report' }
    ];

    // --- Drag Drawing State ---
    let isDrawing = false;
    const scriptNameInput = document.getElementById('scriptName'); // Added: Get reference to script name input
    const saveScriptButton = document.getElementById('saveScriptButton');
    const newScriptButton = document.getElementById('newScriptButton');
    const scriptMgmtStatus = document.getElementById('scriptMgmtStatus');
    const deviceScriptListContainer = document.getElementById('deviceScriptListContainer');
    const userIdInput = document.getElementById('userId'); // Added User ID input
    const scriptListSelect = document.getElementById('scriptList'); // Added: Get reference to script list select
    const loadScriptButton = document.getElementById('loadScriptButton'); // Added: Get reference to load script button

    // Global configuration object
    const globalConfig = {
        enableProfiling: true, // Enable profiling by default
        enableTransformCaching: false,
        enableRepeatOptimization: false,
        enablePixelBatching: false
    };

    // --- NanoID Import & Setup ---
    // Import nanoid
    // Note: Ensure this path is correct relative to your project structure or use a CDN.
    // If running locally, you might need to adjust this or use a bundler.
    // For direct browser use with ES modules, ensure your server serves it correctly.
    let nanoid, customAlphabet;
    try {
        const nanoidModule = await import('https://cdn.jsdelivr.net/npm/nanoid@4.0.2/+esm');
        nanoid = nanoidModule.nanoid; // Default export if available
        customAlphabet = nanoidModule.customAlphabet; // Named export
    } catch (e) {
        console.error("Failed to load nanoid module. User ID generation will be affected.", e);
        // Fallback or error handling if nanoid doesn't load
        // For simplicity, we'll proceed, but ID generation might fail.
    }


    // Define custom nanoid generator
    const NANOID_ALPHABET = "123456789bcdfghjkmnpqrstvwxyz";
    const generatePeerId = customAlphabet ? customAlphabet(NANOID_ALPHABET, 10) : () => `fallback-${Date.now()}-${Math.random().toString(36).substring(2, 8)}`;


    // --- Local Storage Keys ---
    const LOCAL_STORAGE_SCRIPT_CONTENT_KEY = 'micropatterns_editor_content';
    const LOCAL_STORAGE_SCRIPT_NAME_KEY = 'micropatterns_editor_script_name';
    const LOCAL_STORAGE_THEME_KEY = 'micropatterns_theme_preference';
    const LOCAL_STORAGE_USER_ID_KEY = 'micropatterns_user_id'; // Added User ID key

    // --- User ID State ---
    let currentUserId = '';

    // --- Configuration ---
    // Assume server runs on localhost:8000 during development
    // basic detect environment
    let API_BASE_URL;
    if (window.location.hostname === 'localhost' || window.location.hostname.startsWith("127")) {
        API_BASE_URL = 'http://localhost:8000';
    } else {
        API_BASE_URL = 'https://micropatterns-api.deno.dev';
    }
    
    // Compiler optimization configuration
    const optimizationConfig = {
        // Common
        enableTransformCaching: true,      // UI is checked by default
        enablePatternTileCaching: true,    // UI is checked by default
        enablePixelBatching: true,         // UI is checked by default
        logOptimizationStats: false,       // UI is unchecked by default
        logProfilingReport: false,         // UI is unchecked by default

        // Interpreter/Compiler specific
        enableOverdrawOptimization: false, // (Pixel Occupancy) UI is unchecked by default

        // Compiler specific
        enableLoopUnrolling: true,         // UI is checked by default
        loopUnrollThreshold: 8,            // No UI, compiler default is used
        enableInvariantHoisting: true,     // UI is checked by default
        enableFastPathSelection: true,     // UI is checked by default (newly added)
        enableSecondPassOptimization: true,// UI is checked by default
        enableDrawCallBatching: true,      // UI is checked by default (second pass sub-option)
        enableDeadCodeElimination: true,   // UI is checked by default (second pass sub-option)
        enableConstantFolding: true,       // UI is checked by default (second pass sub-option)
        enableTransformSequencing: true,   // UI is checked by default (second pass sub-option)
        enableDrawOrderOptimization: true, // UI is checked by default (second pass sub-option)
        enableMemoryOptimization: true,    // UI is checked by default (second pass sub-option)

        // Display List specific
        enableOcclusionCulling: true,     // (Display List) UI is checked by default, and this config confirms it
        occlusionBlockSize: 16             // Default block size for occlusion buffer
    };
    // --- End Configuration ---

    // --- Drag Drawing State ---
    // let isDrawing = false; // Removed duplicate declaration, already declared earlier
    let drawColor = 0; // 0 for white, 1 for black
    let lastDrawnPixel = { x: -1, y: -1 };
    // --- End Drag Drawing State ---

    const env = {
        HOUR: document.getElementById('envHour'),
        MINUTE: document.getElementById('envMinute'),
        SECOND: document.getElementById('envSecond'),
        COUNTER: document.getElementById('envCounter'),
        WIDTH: 540, // Initial default, will be set by updateCanvasDimensions
        HEIGHT: 960, // Initial default
    };

    let m5PaperZoomFactor = 0.5; // 0.5 for 50% zoom, 1.0 for 100% zoom

    function applyM5PaperZoom() {
        if (canvas.width === 540 && canvas.height === 960) {
            canvas.style.width = (canvas.width * m5PaperZoomFactor) + 'px';
            canvas.style.height = (canvas.height * m5PaperZoomFactor) + 'px';
            zoomToggleButton.textContent = m5PaperZoomFactor === 1.0 ? "Zoom Out (50%)" : "Zoom In (100%)";
        }
    }

    function updateCanvasDimensions(actualWidth, actualHeight) {
        env.WIDTH = actualWidth;
        env.HEIGHT = actualHeight;

        canvas.width = actualWidth; // Set attribute for drawing buffer
        canvas.height = actualHeight; // Set attribute for drawing buffer

        if (actualWidth === 540 && actualHeight === 960) {
            // For M5Paper, apply the current zoom factor for display
            // m5PaperZoomFactor is initialized to 0.5
            applyM5PaperZoom();
            zoomToggleButton.disabled = false;
        } else {
            // For other sizes (e.g., 200x200), display at 100%
            canvas.style.width = actualWidth + 'px';
            canvas.style.height = actualHeight + 'px';
            zoomToggleButton.disabled = true;
            zoomToggleButton.textContent = "Zoom"; // Reset button text
        }

        if (displayInfoSpan) {
            displayInfoSpan.textContent = `${actualWidth}x${actualHeight}`;
        }
        // Update the comment in the new script template if newScript is called later
        // This is handled by newScript() itself as it reads env.WIDTH/HEIGHT
    }

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

    // --- Load from Local Storage ---
    const savedScriptContent = localStorage.getItem(LOCAL_STORAGE_SCRIPT_CONTENT_KEY);
    if (savedScriptContent !== null) {
        codeMirrorEditor.setValue(savedScriptContent);
        console.log("Loaded script content from local storage.");
    } else {
        console.log("No script content found in local storage, using default from textarea.");
    }

    const savedScriptName = localStorage.getItem(LOCAL_STORAGE_SCRIPT_NAME_KEY);
    if (savedScriptName !== null && scriptNameInput) {
        scriptNameInput.value = savedScriptName;
        console.log("Loaded script name from local storage.");
    } else {
        console.log("No script name found in local storage.");
    }

    // --- Save to Local Storage on Change ---
    if (codeMirrorEditor) {
        codeMirrorEditor.on('change', () => {
            localStorage.setItem(LOCAL_STORAGE_SCRIPT_CONTENT_KEY, codeMirrorEditor.getValue());
            // console.log("Saved script content to local storage."); // Can be noisy
        });
    }

    if (scriptNameInput) {
        scriptNameInput.addEventListener('input', () => {
            localStorage.setItem(LOCAL_STORAGE_SCRIPT_NAME_KEY, scriptNameInput.value);
            // console.log("Saved script name to local storage."); // Can be noisy
        });
    }

    // --- User ID Initialization and Handling ---
    function initializeUserId() {
        if (!userIdInput) {
            console.error("User ID input field not found.");
            return;
        }
        const savedUserId = localStorage.getItem(LOCAL_STORAGE_USER_ID_KEY);
        if (savedUserId) {
            currentUserId = savedUserId;
            console.log("Loaded User ID from local storage:", currentUserId);
        } else {
            if (generatePeerId) {
                currentUserId = generatePeerId();
                localStorage.setItem(LOCAL_STORAGE_USER_ID_KEY, currentUserId);
                console.log("Generated new User ID:", currentUserId);
            } else {
                currentUserId = "default-user-id"; // Fallback if nanoid failed
                console.warn("Nanoid not available, using fallback User ID. Please check module import.");
            }
        }
        userIdInput.value = currentUserId;

        userIdInput.addEventListener('input', () => {
            currentUserId = userIdInput.value.trim();
            localStorage.setItem(LOCAL_STORAGE_USER_ID_KEY, currentUserId);
            console.log("User ID updated and saved to local storage:", currentUserId);
            // Optionally, re-fetch script list if user ID changes significantly
            // For now, user needs to manually click load/save after changing ID.
            fetchScriptList(); // Re-fetch script list for the new user ID
        });
    }
    // --- End User ID Initialization ---


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

    // --- Theme Switcher Logic ---
    function applyTheme(themeFile) {
        if (themeStylesheet && themeFile) {
            themeStylesheet.href = themeFile;
            if (themeSelect) { // Sync dropdown if it exists
                themeSelect.value = themeFile;
            }
            localStorage.setItem(LOCAL_STORAGE_THEME_KEY, themeFile);
            console.log(`Theme applied: ${themeFile}`);
        } else {
            console.error("Theme stylesheet link or theme file not found for applyTheme.");
        }
    }

    // Load theme preference on page load
    if (themeStylesheet && themeSelect) { // Ensure elements are available
        const savedTheme = localStorage.getItem(LOCAL_STORAGE_THEME_KEY);
        if (savedTheme) {
            applyTheme(savedTheme); // This will also set themeSelect.value
            console.log(`Loaded theme from local storage: ${savedTheme}`);
        } else {
            // Default to the one set in HTML (style.css) and save it as preference
            const initialTheme = themeStylesheet.getAttribute('href') || 'style.css';
            themeSelect.value = initialTheme; // Ensure select matches
            localStorage.setItem(LOCAL_STORAGE_THEME_KEY, initialTheme);
            console.log(`No saved theme, defaulted to ${initialTheme} and saved preference.`);
        }

        // Add listener for theme changes
        themeSelect.addEventListener('change', (event) => {
            applyTheme(event.target.value);
        });
    } else {
        console.error("Theme select or stylesheet element not found. Theme switching disabled.");
    }
    // --- End Theme Switcher Logic ---
    
    // --- Optimization Settings UI Logic ---
    function setupOptimizationUI() {
        const executionPathRadios = document.querySelectorAll('input[name="executionPath"]');
        const occlusionCullingOptionRow = document.getElementById('occlusionCullingOptionRow');
        const enableOcclusionCullingCheckbox = document.getElementById('enableOcclusionCulling');

        executionPathRadios.forEach(radio => {
            // Set initial state from the global `executionPath` variable
            if (radio.value === executionPath) {
                radio.checked = true;
                if (executionPath === 'displayList' && occlusionCullingOptionRow) {
                    occlusionCullingOptionRow.style.display = ''; // Show occlusion culling option
                } else if (occlusionCullingOptionRow) {
                    occlusionCullingOptionRow.style.display = 'none'; // Hide for other paths
                }
            }

            radio.addEventListener('change', function(e) {
                if (e.target.checked) {
                    executionPath = e.target.value;
                    console.log(`Execution path changed to: ${executionPath}`);
                    if (executionPath === 'displayList' && occlusionCullingOptionRow) {
                        occlusionCullingOptionRow.style.display = '';
                    } else if (occlusionCullingOptionRow) {
                        occlusionCullingOptionRow.style.display = 'none';
                    }
                    // Update UI for compiler-specific options if path changes
                    updateCompilerOptionsUI();
                }
            });
        });
        
        if (enableOcclusionCullingCheckbox) {
            enableOcclusionCullingCheckbox.checked = optimizationConfig.enableOcclusionCulling;
            enableOcclusionCullingCheckbox.addEventListener('change', function(e) {
                optimizationConfig.enableOcclusionCulling = e.target.checked;
                console.log(`Occlusion Culling (Display List) changed to: ${e.target.checked}`);
            });
        }

        // checkboxesConfig is now defined in the outer scope

        checkboxesConfig.forEach(cbConfig => {
            const checkbox = document.getElementById(cbConfig.id);
            if (checkbox) {
                // Set initial checked state from the optimizationConfig object
                checkbox.checked = optimizationConfig[cbConfig.configKey];
                
                // Add event listener to update the optimizationConfig object on change
                checkbox.addEventListener('change', function(e) {
                    optimizationConfig[cbConfig.configKey] = e.target.checked;
                    console.log(`${cbConfig.label} changed to: ${e.target.checked}`);
                });
            } else {
                console.warn(`Checkbox with ID '${cbConfig.id}' not found in HTML.`);
            }
        });

        // After all checkboxes are processed and listeners attached,
        // set the initial state of dependent second-pass options.
        updateSecondPassDependentOptionsUI();
    }
    // --- End Optimization Settings UI Logic ---

    // --- Update UI for Second-Pass Dependent Options ---
    function updateSecondPassDependentOptionsUI() {
        const secondPassCheckbox = document.getElementById('enableSecondPassOptimization');
        if (!secondPassCheckbox) return;

        const isSecondPassEnabled = secondPassCheckbox.checked;

        const dependentOptionIds = [
            'enableDrawCallBatching',
            'enableDeadCodeElimination',
            'enableConstantFolding',
            'enableTransformSequencing',
            'enableDrawOrderOptimization',
            'enableMemoryOptimization'
        ];

        dependentOptionIds.forEach(id => {
            const checkbox = document.getElementById(id);
            if (checkbox) {
                checkbox.disabled = !isSecondPassEnabled;
            }
        });
    }

    // Initial setup for dependent options and listener for the master checkbox
    const enableSecondPassOptimizationCheckbox = document.getElementById('enableSecondPassOptimization');
    if (enableSecondPassOptimizationCheckbox) {
        enableSecondPassOptimizationCheckbox.addEventListener('change', updateSecondPassDependentOptionsUI);
    }
    
    // Function to enable/disable compiler-specific options based on execution path
    function updateCompilerOptionsUI() {
        const isCompilerPath = (executionPath === 'compiler');
        checkboxesConfig.forEach(cbConfig => {
            if (cbConfig.compilerOnly) {
                const checkbox = document.getElementById(cbConfig.id);
                if (checkbox) {
                    checkbox.disabled = !isCompilerPath;
                    // If disabling a master option (like second pass), also update its dependents
                    if (cbConfig.id === 'enableSecondPassOptimization' && !isCompilerPath) {
                        updateSecondPassDependentOptionsUI(); // This will disable dependents
                    }
                }
            }
        });
        // Ensure second-pass dependent options are correctly set based on the master second-pass checkbox
        updateSecondPassDependentOptionsUI();
    }

    // Call initial UI updates after all listeners are attached
    updateCompilerOptionsUI(); // Set initial state for compiler-specific options
    // updateSecondPassDependentOptionsUI(); // Already called by updateCompilerOptionsUI
    // --- End Update UI for Second-Pass Dependent Options ---


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

    // Initial setup of canvas dimensions - M5Paper (540x960) is default in HTML select
    // updateCanvasDimensions will handle the initial 50% zoom for M5Paper.
    if (displaySizeSelect) {
        const initialValue = displaySizeSelect.value; // Should be "540x960"
        const [initialWidth, initialHeight] = initialValue.split('x').map(Number);
        m5PaperZoomFactor = 0.5; // Ensure 50% zoom on initial load for M5Paper
        updateCanvasDimensions(initialWidth, initialHeight);
    } else {
        // Fallback, though select should exist
        updateCanvasDimensions(540, 960); // Default to M5Paper if select somehow missing
    }

    if (zoomToggleButton) {
        zoomToggleButton.addEventListener('click', () => {
            if (canvas.width === 540 && canvas.height === 960) { // Only active for M5Paper
                m5PaperZoomFactor = (m5PaperZoomFactor === 0.5) ? 1.0 : 0.5;
                applyM5PaperZoom();
            }
        });
    }

    if (displaySizeSelect) {
        displaySizeSelect.addEventListener('change', (event) => {
            const [newWidth, newHeight] = event.target.value.split('x').map(Number);
            if (newWidth === 540 && newHeight === 960) {
                m5PaperZoomFactor = 0.5; // Reset to 50% zoom when selecting M5Paper
            }
            updateCanvasDimensions(newWidth, newHeight);
            runScript(); // Re-run script with new dimensions
        });
    }

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

        // `executionPath` is now a global variable updated by UI radio buttons
        console.log(`runScript: executionPath = ${executionPath}`);

        const scriptText = codeMirrorEditor.getValue();
        const environment = getEnvironmentVariables();
        const parser = new MicroPatternsParser();
        let parseResult;
        let hasErrors = false;

        profilingEnabled = optimizationConfig.logProfilingReport === true; // Use UI checkbox for this
        profilingData = {};

        // --- Parsing Phase ---
        try {
            parseResult = parser.parse(scriptText);
            if (parseResult.errors.length > 0) {
                parseResult.errors.forEach(err => displayError(err.message, "Parse"));
                hasErrors = true;
                renderAssetPreviews(parseResult.assets); // Still render previews
                return;
            }
            renderAssetPreviews(parseResult.assets);
        } catch (e) {
            displayError(`Unexpected Parser Crash: ${e.message}`, "Fatal Parse");
            console.error(e);
            hasErrors = true;
            return;
        }

        // Reset instances for the current run
        currentRuntimeInstance = null;
        currentCompilerInstance = null;
        currentCompiledRunnerInstance = null;
        currentDisplayListGenerator = null;
        currentDisplayListRenderer = null;

        // --- Runtime Phase ---
        if (executionPath === 'compiler') {
            console.log("Taking Compiler Path");
            // Compiler Path logic (mostly unchanged, ensure optimizationConfig is passed correctly)
            if (optimizationConfig.enableSecondPassOptimization) {
                // Logic to adjust sub-options based on their checkboxes (already in your existing code)
            }
            console.log("Compiler using optimization settings:", JSON.stringify(optimizationConfig, null, 2));
            currentCompilerInstance = new MicroPatternsCompiler(optimizationConfig);
            let compiledOutput;
            if (profilingEnabled) wrapForProfiling(currentCompilerInstance, 'compile', profilingData);
            try {
                compiledOutput = currentCompilerInstance.compile(parseResult.commands, parseResult.assets.assets, environment);
                if (compiledOutput.errors && compiledOutput.errors.length > 0) {
                    compiledOutput.errors.forEach(err => displayError(err.message || err, "Compiler"));
                    hasErrors = true;
                }
            } catch (e) {
                displayError(`Compiler Crash: ${e.message}`, "Fatal Compiler");
                console.error(e); hasErrors = true; return;
            }
            if (hasErrors && !compiledOutput.execute) { displayProfilingResults(); return; }

            currentCompiledRunnerInstance = new MicroPatternsCompiledRunner(ctx, (msg) => { displayError(msg, "CompiledRuntime"); hasErrors = true; }, errorLog);
            if (profilingEnabled) wrapForProfiling(currentCompiledRunnerInstance, 'execute', profilingData);
            try {
                currentCompiledRunnerInstance.execute(compiledOutput, parseResult.assets.assets, environment);
            } catch (e) {
                console.error("Unhandled Compiled Execution Exception:", e);
                displayError(`Unexpected Compiled Execution Crash: ${e.message}`, "Fatal CompiledRuntime");
                hasErrors = true;
            }

        } else if (executionPath === 'displayList') {
            console.log("Taking Display List Path");
            // --- Display List Path ---
            currentDisplayListGenerator = new DisplayListGenerator(parseResult.assets.assets, environment);
            // The parser produces `parseResult.variables` (a Set of declared var names).
            // The generator needs initial values for these (default to 0).
            const initialUserVariablesForGenerator = {};
            parseResult.variables.forEach(varNameUpper => {
                initialUserVariablesForGenerator[`$${varNameUpper}`] = 0;
            });

            if (profilingEnabled) wrapForProfiling(currentDisplayListGenerator, 'generate', profilingData);
            
            const generatorOutput = currentDisplayListGenerator.generate(parseResult.commands, initialUserVariablesForGenerator);

            if (generatorOutput.errors.length > 0) {
                generatorOutput.errors.forEach(err => displayError(err, "DisplayListGenerator"));
                hasErrors = true;
            }

            if (hasErrors) { displayProfilingResults(); return; }

            // Pass relevant parts of optimizationConfig to the renderer
            const rendererOptimizationConfig = {
                enableOcclusionCulling: optimizationConfig.enableOcclusionCulling,
                occlusionBlockSize: optimizationConfig.occlusionBlockSize,
                enablePixelBatching: optimizationConfig.enablePixelBatching, // For renderer's own batching
                // Pass other flags if DisplayListRenderer or its MicroPatternsDrawing instance uses them
                enablePatternTileCaching: optimizationConfig.enablePatternTileCaching,
                enableTransformCaching: optimizationConfig.enableTransformCaching, // If drawing methods use it
            };
            currentDisplayListRenderer = new DisplayListRenderer(ctx, parseResult.assets.assets, rendererOptimizationConfig);
            
            if (profilingEnabled) wrapForProfiling(currentDisplayListRenderer, 'render', profilingData);

            try {
                currentDisplayListRenderer.render(generatorOutput.displayList);
                // Log Display List stats
                const dlStats = currentDisplayListRenderer.getStats();
                let statsReport = "\n--- Display List Stats ---\n";
                statsReport += `Total Items: ${dlStats.totalItems}\n`;
                statsReport += `Rendered Items: ${dlStats.renderedItems}\n`;
                statsReport += `Culled (Off-Screen): ${dlStats.culledOffScreen}\n`;
                statsReport += `Culled (Occlusion): ${dlStats.culledByOcclusion}\n`;
                if (optimizationConfig.enableOcclusionCulling) {
                    statsReport += `Occlusion Buffer: ${dlStats.occlusionBufferStats.gridWidth}x${dlStats.occlusionBufferStats.gridHeight} blocks (size ${dlStats.occlusionBufferStats.blockSize}px)\n`;
                }
                console.log(statsReport);
                if (errorLog) errorLog.textContent += statsReport;

            } catch (e) {
                console.error("Unhandled Display List Renderer Exception:", e);
                displayError(`Unexpected Display List Renderer Crash: ${e.message}`, "Fatal DisplayListRenderer");
                hasErrors = true;
            }

        } else { // Interpreter Path (default)
            console.log("Taking Interpreter Path");
            // --- Interpreter Path ---
            currentRuntimeInstance = new MicroPatternsRuntime(ctx, parseResult.assets, environment, (msg) => { displayError(msg, "Runtime"); hasErrors = true; });
            if (profilingEnabled) {
                // Profiling setup for interpreter (as before)
                const runtimeMethodsToProfile = ['executeCommand', 'evaluateExpression', 'evaluateCondition', '_resolveValue'];
                runtimeMethodsToProfile.forEach(method => wrapForProfiling(currentRuntimeInstance, method, profilingData));
                if (currentRuntimeInstance.drawing) {
                    const drawingMethodsToProfile = [
                        'drawLine', 'drawRect', 'fillRect', 'drawPixel', 'fillCircle', 'drawCircle',
                        'drawFilledPixel', 'drawAsset', 'transformPoint', 'screenToLogicalBase',
                        '_getFillAssetPixelColor', 'setPixel', '_rawLine'
                    ];
                    drawingMethodsToProfile.forEach(method => wrapForProfiling(currentRuntimeInstance.drawing, method, profilingData));
                }
            }
            // Pass relevant optimization flags to runtime if it uses them
            // For example, if runtime.drawing uses enablePixelBatching from optimizationConfig
            if (currentRuntimeInstance.drawing && currentRuntimeInstance.drawing.setOptimizationConfig) {
                 currentRuntimeInstance.drawing.setOptimizationConfig({
                    enablePixelBatching: optimizationConfig.enablePixelBatching,
                    enableOverdrawOptimization: optimizationConfig.enableOverdrawOptimization,
                    enableTransformCaching: optimizationConfig.enableTransformCaching,
                    enablePatternTileCaching: optimizationConfig.enablePatternTileCaching,
                 });
            }

            try {
                currentRuntimeInstance.execute(parseResult.commands);
            } catch (e) {
                if (!e.isRuntimeError) {
                    displayError(`Unexpected Interpreter Crash: ${e.message}`, "Fatal Interpreter");
                    console.error("Unhandled Interpreter Exception:", e);
                    hasErrors = true;
                }
            }
        }
        // --- End Runtime Phase ---

        // Display profiling results if enabled
        if (profilingEnabled) {
            displayProfilingResults();
        }

        // Auto-increment counter if no errors occurred and counter is not locked
        if (!hasErrors && !isCounterLocked) {
            env.COUNTER.value = parseInt(env.COUNTER.value, 10) + 1;
        }
    }

    runButton.addEventListener('click', runScript);

    if (lockCounterButton) {
        // Initial state is unlocked. HTML sets initial SVG structure.
        // CSS handles showing the unlocked icon by default.
        // Title is set in HTML and confirmed here.
        lockCounterButton.title = 'Counter is unlocked (click to lock)';

        lockCounterButton.addEventListener('click', () => {
            isCounterLocked = !isCounterLocked;
            if (isCounterLocked) { // Counter is NOW locked
                lockCounterButton.classList.add('is-locked');
                lockCounterButton.title = 'Counter is locked (click to unlock)';
                env.COUNTER.classList.add('locked-counter');
            } else { // Counter is NOW unlocked
                lockCounterButton.classList.remove('is-locked');
                lockCounterButton.title = 'Counter is unlocked (click to lock)';
                env.COUNTER.classList.remove('locked-counter');
            }
            // Note: Clicking the lock button does not re-run the script.
        });
    }

    resetCounterButton.addEventListener('click', () => {
        env.COUNTER.value = 0; // Reset counter to zero
        // Don't run script after resetting as per requirements
    });

    // --- Asset Preview Rendering and Editing ---

    const PREVIEW_SCALE = 12; // How many screen pixels per asset pixel

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
        canvas.dataset.assetName = asset.name; // Use uppercase name for lookup
        canvas.dataset.assetType = 'PATTERN'; // Store asset type

        // --- Add Drag Drawing Event Listeners ---
        canvas.addEventListener('mousedown', (event) => handleMouseDown(event, canvas, asset));
        canvas.addEventListener('mousemove', (event) => handleMouseMove(event, canvas, asset));
        canvas.addEventListener('mouseup', (event) => handleMouseUpOrLeave(canvas, asset));
        canvas.addEventListener('mouseleave', (event) => handleMouseUpOrLeave(canvas, asset));
        // Prevent drag selection of the canvas itself
        canvas.addEventListener('dragstart', (event) => event.preventDefault());


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
        reader.onload = function (e) {
            const img = new Image();
            img.onload = function () {
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
            img.onerror = function () {
                displayError(`Error loading dropped image for ${targetAsset.name}.`, "Drop Error");
            };
            img.src = e.target.result; // Set image source to the data URL
        };
        reader.onerror = function () {
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


    // --- Drag Drawing Handlers ---

    // Helper to draw a single pixel on the preview canvas
    function drawSinglePixelOnPreview(ctx, x, y, colorValue, scale) {
        ctx.fillStyle = colorValue === 1 ? 'black' : 'white';
        ctx.fillRect(x * scale, y * scale, scale, scale);
        // Redraw grid line potentially covered by the pixel fill
        ctx.strokeStyle = '#ddd';
        ctx.lineWidth = 1;
        // Vertical line to the right
        ctx.beginPath();
        ctx.moveTo((x + 1) * scale, y * scale);
        ctx.lineTo((x + 1) * scale, (y + 1) * scale);
        ctx.stroke();
        // Horizontal line below
        ctx.beginPath();
        ctx.moveTo(x * scale, (y + 1) * scale);
        ctx.lineTo((x + 1) * scale, (y + 1) * scale);
        ctx.stroke();
        // Vertical line to the left (needed if x=0)
        ctx.beginPath();
        ctx.moveTo(x * scale, y * scale);
        ctx.lineTo(x * scale, (y + 1) * scale);
        ctx.stroke();
        // Horizontal line above (needed if y=0)
        ctx.beginPath();
        ctx.moveTo(x * scale, y * scale);
        ctx.lineTo((x + 1) * scale, y * scale);
        ctx.stroke();
    }


    function handleMouseDown(event, canvas, asset) {
        isDrawing = true;
        const rect = canvas.getBoundingClientRect();
        const clickX = event.clientX - rect.left;
        const clickY = event.clientY - rect.top;

        const assetX = Math.floor(clickX / PREVIEW_SCALE);
        const assetY = Math.floor(clickY / PREVIEW_SCALE);

        if (assetX >= 0 && assetX < asset.width && assetY >= 0 && assetY < asset.height) {
            const index = assetY * asset.width + assetX;
            // Toggle the pixel value
            asset.data[index] = 1 - asset.data[index];
            drawColor = asset.data[index]; // Store the new color (0 or 1)
            lastDrawnPixel = { x: assetX, y: assetY };

            // Redraw just the clicked pixel on the canvas
            const ctx = canvas.getContext('2d');
            drawSinglePixelOnPreview(ctx, assetX, assetY, drawColor, PREVIEW_SCALE);
        } else {
            // Clicked outside bounds, don't start drawing
            isDrawing = false;
        }
    }

    function handleMouseMove(event, canvas, asset) {
        if (!isDrawing) return;

        const rect = canvas.getBoundingClientRect();
        const moveX = event.clientX - rect.left;
        const moveY = event.clientY - rect.top;

        const assetX = Math.floor(moveX / PREVIEW_SCALE);
        const assetY = Math.floor(moveY / PREVIEW_SCALE);

        // Check bounds and if it's a new pixel
        if (assetX >= 0 && assetX < asset.width && assetY >= 0 && assetY < asset.height) {
            if (assetX !== lastDrawnPixel.x || assetY !== lastDrawnPixel.y) {
                const index = assetY * asset.width + assetX;
                // Only draw if the pixel isn't already the target color
                if (asset.data[index] !== drawColor) {
                    asset.data[index] = drawColor; // Set pixel to the stored draw color

                    // Redraw just this pixel on the canvas
                    const ctx = canvas.getContext('2d');
                    drawSinglePixelOnPreview(ctx, assetX, assetY, drawColor, PREVIEW_SCALE);
                }
                lastDrawnPixel = { x: assetX, y: assetY }; // Update last drawn position
            }
        } else {
            // Moved out of bounds, treat as end of stroke for this pixel
            lastDrawnPixel = { x: -1, y: -1 };
        }
    }

    function handleMouseUpOrLeave(canvas, asset) {
        if (isDrawing) {
            isDrawing = false;
            lastDrawnPixel = { x: -1, y: -1 }; // Reset last drawn pixel

            // Update the CodeMirror editor with the final data AFTER the drag is complete
            updateCodeMirrorAssetData(canvas.dataset.assetType, asset.name, asset.data);
        }
    }

    // --- End Drag Drawing Handlers ---


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
        if (!currentUserId) {
            setStatusMessage("User ID is not set. Cannot load scripts.", true);
            scriptListSelect.options.length = 1; // Clear list
            if (deviceScriptListContainer) deviceScriptListContainer.innerHTML = '<p style="color: #777; font-style: italic;">User ID required to load scripts.</p>';
            return;
        }
        console.log(`Fetching script list for user: ${currentUserId}...`);
        setStatusMessage("Loading script list...");
        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts/${currentUserId}`);
            if (!response.ok) {
                if (response.status === 404) { // User might not have any scripts yet
                    console.log(`No scripts found for user ${currentUserId}. This might be a new user.`);
                    scriptListSelect.options.length = 1; // Clear previous options
                    populateDeviceScriptList([], []); // Empty device list
                    setStatusMessage("No scripts found for this User ID.", false);
                    return;
                }
                throw new Error(`Failed to fetch script list: ${response.status} ${response.statusText}`);
            }
            const scripts = await response.json();

            // Clear existing options (except the default)
            scriptListSelect.options.length = 1;

            // Populate dropdown for editor loading
            scripts.forEach(script => {
                const option = document.createElement('option');
                option.value = script.id;
                option.textContent = script.name;
                scriptListSelect.appendChild(option);
            });
            setStatusMessage("Script list loaded.", false);
            console.log("Script list loaded:", scripts);

            // Fetch device-specific script list to populate checkboxes
            try {
                const deviceScriptsResponse = await fetch(`${API_BASE_URL}/api/device/scripts/${currentUserId}`);
                if (!deviceScriptsResponse.ok) {
                    if (deviceScriptsResponse.status === 404) {
                        console.log(`No device-specific script list found for user ${currentUserId}.`);
                        populateDeviceScriptList(scripts, []);
                        return;
                    }
                    throw new Error(`Failed to fetch device script list: ${deviceScriptsResponse.status} ${deviceScriptsResponse.statusText}`);
                }
                const deviceScriptsArray = await deviceScriptsResponse.json();
                const deviceScriptIds = deviceScriptsArray.map(s => s.id);
                populateDeviceScriptList(scripts, deviceScriptIds);
            } catch (deviceError) {
                setStatusMessage(`Error loading device script selection: ${deviceError.message}`, true);
                populateDeviceScriptList(scripts, []);
            }

        } catch (error) {
            setStatusMessage(`Error loading script list: ${error.message}`, true);
            scriptListSelect.options.length = 1; // Clear list on error
            if (deviceScriptListContainer) deviceScriptListContainer.innerHTML = '<p style="color: red; font-style: italic;">Error loading script list.</p>';
        }
    }

    function populateDeviceScriptList(allScripts, deviceScriptIds) {
        if (!deviceScriptListContainer) return;
        deviceScriptListContainer.innerHTML = ''; // Clear previous

        if (allScripts.length === 0) {
            deviceScriptListContainer.innerHTML = '<p style="color: #777; font-style: italic;">No scripts available.</p>';
            return;
        }

        allScripts.forEach(script => {
            const itemDiv = document.createElement('div');
            itemDiv.style.marginBottom = '3px';

            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.id = `device-script-${script.id}`;
            checkbox.value = script.id;
            checkbox.checked = deviceScriptIds.includes(script.id);
            checkbox.addEventListener('change', updateDeviceSelection);

            const label = document.createElement('label');
            label.htmlFor = checkbox.id;
            label.textContent = script.name;
            label.style.marginLeft = '5px';
            label.style.cursor = 'pointer';


            itemDiv.appendChild(checkbox);
            itemDiv.appendChild(label);
            deviceScriptListContainer.appendChild(itemDiv);
        });
    }

    async function updateDeviceSelection() {
        if (!currentUserId) {
            setStatusMessage("User ID is not set. Cannot update device selection.", true);
            return;
        }
        if (!deviceScriptListContainer) return;
        const selectedIds = [];
        const checkboxes = deviceScriptListContainer.querySelectorAll('input[type="checkbox"]');
        checkboxes.forEach(cb => {
            if (cb.checked) {
                selectedIds.push(cb.value);
            }
        });

        console.log(`Updating device selection for user ${currentUserId} with IDs:`, selectedIds);
        setStatusMessage("Updating device selection...");

        try {
            const response = await fetch(`${API_BASE_URL}/api/device/scripts/${currentUserId}`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ selectedIds: selectedIds }) // Server will use userID from path
            });

            if (!response.ok) {
                const errorData = await response.json().catch(() => ({ error: 'Unknown error updating device selection' }));
                throw new Error(`Failed: ${response.status} ${response.statusText} - ${errorData.error || ''}`);
            }
            setStatusMessage("Device script selection updated.", false);
        } catch (error) {
            setStatusMessage(`Error updating device selection: ${error.message}`, true);
        }
    }


    async function loadScript(scriptId) {
        if (!currentUserId) {
            setStatusMessage("User ID is not set. Cannot load script.", true);
            return;
        }
        if (!scriptId) {
            setStatusMessage("Please select a script to load.", true);
            return;
        }
        console.log(`Loading script: ${scriptId} for user ${currentUserId}`);
        setStatusMessage(`Loading script '${scriptId}'...`);

        // Reset counter and overrides BEFORE loading
        resetEnvironmentInputs();

        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts/${currentUserId}/${scriptId}`);
            if (!response.ok) {
                if (response.status === 404) {
                    throw new Error(`Script '${scriptId}' not found for this user.`);
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
        if (!currentUserId) {
            setStatusMessage("User ID is not set. Cannot save script.", true);
            return;
        }
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

        console.log(`Saving script: ID=${scriptId}, Name=${scriptName} for user ${currentUserId}`);
        setStatusMessage(`Saving script '${scriptName}'...`);

        try {
            const response = await fetch(`${API_BASE_URL}/api/scripts/${currentUserId}/${scriptId}`, {
                method: 'PUT',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    name: scriptName,
                    content: scriptContent,
                    // userId: currentUserId // Server will get userID from path
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

    function createConfirmationDialog(message, options) {
        // Create overlay
        const overlay = document.createElement('div');
        overlay.style.position = 'fixed';
        overlay.style.top = '0';
        overlay.style.left = '0';
        overlay.style.width = '100%';
        overlay.style.height = '100%';
        overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
        overlay.style.display = 'flex';
        overlay.style.justifyContent = 'center';
        overlay.style.alignItems = 'center';
        overlay.style.zIndex = '9999';

        // Create dialog
        const dialog = document.createElement('div');
        dialog.style.backgroundColor = 'var(--bg-color-alt, white)';
        dialog.style.border = '3px solid var(--border-color, black)';
        dialog.style.borderRadius = 'var(--border-radius-slight, 4px)';
        dialog.style.boxShadow = 'var(--shadow-strong, 4px 4px 0px black)';
        dialog.style.padding = '20px';
        dialog.style.maxWidth = '450px';
        dialog.style.width = '90%';
        dialog.style.textAlign = 'center';
        dialog.style.position = 'relative';

        // Add Memphis-style decorative element if using Memphis theme
        const decorElement = document.createElement('div');
        decorElement.style.position = 'absolute';
        decorElement.style.top = '-10px';
        decorElement.style.right = '-10px';
        decorElement.style.width = '20px';
        decorElement.style.height = '20px';
        decorElement.style.backgroundColor = 'var(--accent-color-yellow, yellow)';
        decorElement.style.border = '2px solid var(--border-color, black)';
        decorElement.style.transform = 'rotate(45deg)';
        decorElement.style.zIndex = '-1';
        dialog.appendChild(decorElement);

        // Add message
        const messageElement = document.createElement('p');
        messageElement.textContent = message;
        messageElement.style.marginBottom = '20px';
        messageElement.style.fontSize = '1rem';
        dialog.appendChild(messageElement);

        // Add buttons container
        const buttonsContainer = document.createElement('div');
        buttonsContainer.style.display = 'flex';
        buttonsContainer.style.justifyContent = 'center';
        buttonsContainer.style.gap = '10px';
        buttonsContainer.style.flexWrap = 'wrap';
        dialog.appendChild(buttonsContainer);

        // Create a promise to return
        return new Promise(resolve => {
            // Add buttons based on options
            options.forEach(option => {
                const button = document.createElement('button');
                button.textContent = option.label;
                button.className = option.primary ? 'primary' : 'secondary';

                // Apply theme-consistent styling
                if (option.primary) {
                    button.style.backgroundColor = 'var(--accent-color-green, green)';
                } else if (option.destructive) {
                    button.style.backgroundColor = 'var(--error-color-bg, red)';
                }

                button.addEventListener('click', () => {
                    document.body.removeChild(overlay);
                    resolve(option.value);
                });
                buttonsContainer.appendChild(button);
            });

            // Add dialog to overlay and overlay to body
            overlay.appendChild(dialog);
            document.body.appendChild(overlay);
        });
    }

    async function newScript() {
        // Check if there's content in the editor that might be worth saving
        const currentContent = codeMirrorEditor.getValue().trim();
        const defaultContent = `# New MicroPatterns Script\n# Display is ${env.WIDTH}x${env.HEIGHT}\n\nDEFINE PATTERN NAME="stripes" WIDTH=8 HEIGHT=8 DATA="1111111100000000111111110000000011111111000000001111111100000000"\n\nCOLOR NAME=BLACK\nFILL NAME="stripes"\nFILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT\n\n`.trim();

        // If editor is empty or contains only the default template, no need to confirm
        if (currentContent === '' || currentContent === defaultContent) {
            createNewScript();
            return;
        }

        // Check if the script has a name
        const hasScriptName = scriptNameInput.value.trim() !== '';

        // Prepare dialog options based on whether script has a name
        let dialogMessage, dialogOptions;

        if (hasScriptName) {
            dialogMessage = "Do you want to save your current script before creating a new one?";
            dialogOptions = [
                { label: "Save & New", value: "save", primary: true },
                { label: "Discard & New", value: "discard", destructive: true },
                { label: "Cancel", value: "cancel" }
            ];
        } else {
            dialogMessage = "Your script doesn't have a name. What would you like to do?";
            dialogOptions = [
                { label: "Name & Save", value: "name", primary: true },
                { label: "Discard & New", value: "discard", destructive: true },
                { label: "Cancel", value: "cancel" }
            ];
        }

        // Show confirmation dialog
        const result = await createConfirmationDialog(dialogMessage, dialogOptions);

        switch (result) {
            case "save":
                // First save the current script
                await saveScript();
                // Then create new script
                createNewScript();
                break;
            case "name":
                // Focus on the name input field to prompt user to enter a name
                scriptNameInput.focus();
                // Highlight the field to make it obvious
                scriptNameInput.classList.add('highlight-input');
                // Remove highlight after a delay
                setTimeout(() => {
                    scriptNameInput.classList.remove('highlight-input');
                }, 2000);
                // Show a message
                setStatusMessage("Please enter a name for your script before saving.", true);
                break;
            case "discard":
                // Create new script without saving
                createNewScript();
                break;
            case "cancel":
            default:
                // Do nothing, keep current script
                break;
        }
    }

    function createNewScript() {
        scriptNameInput.value = '';
        codeMirrorEditor.setValue(`# New MicroPatterns Script\n# Display is ${env.WIDTH}x${env.HEIGHT}

DEFINE PATTERN NAME="stripes" WIDTH=8 HEIGHT=8 DATA="1111111100000000111111110000000011111111000000001111111100000000"

VAR $CENTERX = $WIDTH / 2
VAR $CENTERY = $HEIGHT / 2
VAR $SECONDPLUSONE = $SECOND + 1

COLOR NAME=BLACK
FILL NAME="stripes"
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
`);
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
    // --- End Script Management Logic ---

    // --- Profiling System ---
    function displayProfilingResults() {
        // Check if profiling is enabled AND if the report logging is enabled via UI
        if (!profilingEnabled || !optimizationConfig.logProfilingReport) return;

        let report = "--- Profiling Report ---\n";
        const sortedData = Object.entries(profilingData).sort(([, a], [, b]) => b.totalTime - a.totalTime);

        for (const [methodName, stats] of sortedData) {
            const avgTime = stats.totalTime / stats.calls;
            report += `${methodName}: ${stats.calls} calls, ${stats.totalTime.toFixed(2)}ms total, ${avgTime.toFixed(3)}ms avg, ${stats.minTime.toFixed(3)}ms min, ${stats.maxTime.toFixed(3)}ms max\n`;
            
            if (methodName === 'execute' && currentCompiledRunnerInstance && currentCompiledRunnerInstance.executionStats) {
                const execStats = currentCompiledRunnerInstance.executionStats;
                report += "\n  --- Compiler Execution Breakdown ---\n"; // Clarify it's for compiler
                report += `  Display Reset: ${execStats.resetTime.toFixed(2)}ms (${(execStats.resetTime / execStats.totalTime * 100).toFixed(1)}%)\n`;
                report += `  Script Execution: ${execStats.compiledFunctionTime.toFixed(2)}ms (${(execStats.compiledFunctionTime / execStats.totalTime * 100).toFixed(1)}%)\n`;
                report += `  Drawing Operations: ${execStats.drawingOperationsTime.toFixed(2)}ms (${(execStats.drawingOperationsTime / execStats.totalTime * 100).toFixed(1)}%)\n`;
                report += `  Optimization Operations: ${execStats.optimizationTime.toFixed(2)}ms (${(execStats.optimizationTime / execStats.totalTime * 100).toFixed(1)}%)\n`;
                report += `  Final Batch Flush: ${execStats.flushBatchTime.toFixed(2)}ms (${(execStats.flushBatchTime / execStats.totalTime * 100).toFixed(1)}%)\n`;
                
                report += "\n  --- Drawing Operation Counts ---\n";
                report += `  Total Drawing Operations: ${execStats.drawingOperationCounts.total}\n`;
                report += `  Pixels: ${execStats.drawingOperationCounts.pixel}\n`;
                report += `  Lines: ${execStats.drawingOperationCounts.line}\n`;
                report += `  Rectangles: ${execStats.drawingOperationCounts.rect}\n`;
                report += `  Filled Rectangles: ${execStats.drawingOperationCounts.fillRect}\n`;
                report += `  Circles: ${execStats.drawingOperationCounts.circle}\n`;
                report += `  Filled Circles: ${execStats.drawingOperationCounts.fillCircle}\n`;
                report += `  Pattern Draws: ${execStats.drawingOperationCounts.draw}\n`;
                report += `  Filled Pixels: ${execStats.drawingOperationCounts.fillPixel}\n`;
                report += `  Transformations: ${execStats.drawingOperationCounts.transform}\n`;
            } else if (methodName === 'render' && currentDisplayListRenderer) {
                // Add stats from DisplayListRenderer if available
                const dlStats = currentDisplayListRenderer.getStats();
                report += "  --- Display List Rendering Stats ---\n";
                report += `  Total Items: ${dlStats.totalItems}\n`;
                report += `  Rendered Items: ${dlStats.renderedItems}\n`;
                report += `  Culled (Off-Screen): ${dlStats.culledOffScreen}\n`;
                report += `  Culled (Occlusion): ${dlStats.culledByOcclusion}\n`;
                 if (optimizationConfig.enableOcclusionCulling) {
                    report += `  Occlusion Buffer: ${dlStats.occlusionBufferStats.gridWidth}x${dlStats.occlusionBufferStats.gridHeight} blocks (size ${dlStats.occlusionBufferStats.blockSize}px)\n`;
                }
            }
        }

        console.log(report);
        errorLog.textContent += "\n" + report;
    }

    // Ensure wrapForProfiling is defined before it's potentially called in runScript
    function wrapForProfiling(instance, methodName, dataStore) {
        if (!instance || typeof instance[methodName] !== 'function') return;

        const originalMethod = instance[methodName];
        instance[methodName] = function (...args) {
            if (!profilingEnabled) {
                return originalMethod.apply(this, args);
            }

            const start = performance.now();
            const result = originalMethod.apply(this, args);
            const duration = performance.now() - start;

            if (!dataStore[methodName]) {
                dataStore[methodName] = { calls: 0, totalTime: 0, maxTime: 0, minTime: duration };
            }
            dataStore[methodName].calls++;
            dataStore[methodName].totalTime += duration;
            dataStore[methodName].maxTime = Math.max(dataStore[methodName].maxTime, duration);
            dataStore[methodName].minTime = Math.min(dataStore[methodName].minTime, duration);

            return result;
        };
    }
    // --- End Profiling System ---

    // Run once on load
    initializeUserId(); // Initialize User ID first
    setupOptimizationUI(); // Set up optimization UI controls
    runScript();
    fetchScriptList(); // Fetch scripts when the page loads (will use currentUserId)
});