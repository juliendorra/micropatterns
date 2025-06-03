import { MicroPatternsCompiler } from './compiler.js';
import { MicroPatternsCompiledRunner } from './compiled_runtime.js';
import { MicroPatternsParser } from './parser.js';
import { MicroPatternsRuntime } from './runtime.js';
import { DisplayListGenerator } from './display_list_generator.js';
import { DisplayListRenderer } from './display_list_renderer.js';

document.addEventListener('DOMContentLoaded', async () => {
    const urlParams = new URLSearchParams(window.location.search);
    const viewPublishID = urlParams.get('view');
    const isViewMode = !!viewPublishID;

    let executionPath = 'displayList';
    let currentRuntimeInstance = null;
    let currentCompilerInstance = null;
    let currentCompiledRunnerInstance = null;
    let currentDisplayListGenerator = null;
    let currentDisplayListRenderer = null;

    let profilingEnabled = false;
    let profilingData = {};
    
    let isCounterLocked = false;
    
    const scriptInputTextArea = document.getElementById('scriptInput');
    const runButton = document.getElementById('runButton');
    const lockCounterButton = document.getElementById('lockCounterButton');
    const resetCounterButton = document.getElementById('resetCounterButton');
    const canvas = document.getElementById('displayCanvas');
    const ctx = canvas.getContext('2d');
    const errorLog = document.getElementById('errorLog');
    const assetPreviewsContainer = document.getElementById('assetPreviews');
    const realTimeHourSpan = document.getElementById('realTimeHourSpan');
    const realTimeMinuteSpan = document.getElementById('realTimeMinuteSpan');
    const realTimeSecondSpan = document.getElementById('realTimeSecondSpan');
    const lineWrapToggle = document.getElementById('lineWrapToggle');

    const displaySizeSelect = document.getElementById('displaySizeSelect');
    const displayInfoSpan = document.getElementById('displayInfoSpan');
    const zoomToggleButton = document.getElementById('zoomToggleButton');
    const editorTitleElement = document.getElementById('editorTitle');

    const themeStylesheet = document.getElementById('themeStylesheet');
    const themeSelect = document.getElementById('themeSelect');

    const checkboxesConfig = [
        { id: 'enableOcclusionCulling', configKey: 'enableOcclusionCulling', label: 'Occlusion Culling', paths: ['displayList'], rowId: 'occlusionCullingOptionRow' },
        { id: 'enableOverdrawOptimization', configKey: 'enableOverdrawOptimization', label: 'Overdraw Optimization (Pixel Occupancy)', paths: ['compiler'] },
        { id: 'enableTransformCaching', configKey: 'enableTransformCaching', label: 'Transform Caching', paths: ['compiler'] },
        { id: 'enablePatternTileCaching', configKey: 'enablePatternTileCaching', label: 'Pattern Caching', paths: ['interpreter', 'compiler'] },
        { id: 'enablePixelBatching', configKey: 'enablePixelBatching', label: 'Pixel Batching', paths: ['compiler'] },
        { id: 'enableLoopUnrolling', configKey: 'enableLoopUnrolling', label: 'Loop Unrolling', paths: ['compiler'] },
        { id: 'enableInvariantHoisting', configKey: 'enableInvariantHoisting', label: 'Invariant Hoisting', paths: ['compiler'] },
        { id: 'enableFastPathSelection', configKey: 'enableFastPathSelection', label: 'Fast Path Selection', paths: ['compiler'] },
        { id: 'enableSecondPassOptimization', configKey: 'enableSecondPassOptimization', label: 'Second-Pass Optimization', paths: ['compiler'] },
        { id: 'enableDrawCallBatching', configKey: 'enableDrawCallBatching', label: 'Draw Call Batching', paths: ['compiler'] },
        { id: 'enableDeadCodeElimination', configKey: 'enableDeadCodeElimination', label: 'Dead Code Elimination', paths: ['compiler'] },
        { id: 'enableConstantFolding', configKey: 'enableConstantFolding', label: 'Constant Folding', paths: ['compiler'] },
        { id: 'enableTransformSequencing', configKey: 'enableTransformSequencing', label: 'Transform Sequencing', paths: ['compiler'] },
        { id: 'enableDrawOrderOptimization', configKey: 'enableDrawOrderOptimization', label: 'Draw Order Optimization', paths: ['compiler'] },
        { id: 'enableMemoryOptimization', configKey: 'enableMemoryOptimization', label: 'Memory Optimization', paths: ['compiler'] },
        { id: 'logOptimizationStats', configKey: 'logOptimizationStats', label: 'Log Optimization Stats', paths: ['logging'] },
        { id: 'logProfilingReport', configKey: 'logProfilingReport', label: 'Log Profiling Report', paths: ['logging'] }
    ];

    let isDrawing = false;
    const scriptNameInput = document.getElementById('scriptName');
    const saveScriptButton = document.getElementById('saveScriptButton');
    const newScriptButton = document.getElementById('newScriptButton');
    const scriptMgmtStatus = document.getElementById('scriptMgmtStatus');
    const deviceScriptListContainer = document.getElementById('deviceScriptListContainer');
    const userIdInput = document.getElementById('userId');
    const scriptListSelect = document.getElementById('scriptList');
    const loadScriptButton = document.getElementById('loadScriptButton');
    const publishStatusContainer = document.getElementById('publishStatusContainer');

    const globalConfig = {
        enableProfiling: true,
        enableTransformCaching: false,
        enableRepeatOptimization: false,
        enablePixelBatching: false
    };

    let nanoid, customAlphabet;
    try {
        const nanoidModule = await import('https://cdn.jsdelivr.net/npm/nanoid@4.0.2/+esm');
        nanoid = nanoidModule.nanoid;
        customAlphabet = nanoidModule.customAlphabet;
    } catch (e) {
        console.error("Failed to load nanoid module. User ID generation will be affected.", e);
    }

    const NANOID_ALPHABET = "123456789bcdfghjkmnpqrstvwxyz";
    const generatePeerId = customAlphabet ? customAlphabet(NANOID_ALPHABET, 10) : () => `fallback-${Date.now()}-${Math.random().toString(36).substring(2, 8)}`;

    const LOCAL_STORAGE_THEME_KEY = 'micropatterns_theme_preference';
    const LOCAL_STORAGE_USER_ID_KEY = 'micropatterns_user_id';

    let currentUserId = '';
    let currentScriptID = null;
    let currentPublishID = null;
    let currentIsPublished = false; // Added for isPublished state
    let hasUnsavedChanges = false;

    const LS_UNSAVED_CONTENT_KEY = 'micropatterns_editor_unsaved_content';
    const LS_UNSAVED_NAME_KEY = 'micropatterns_editor_unsaved_name';
    const LS_SCRIPT_PREFIX = 'micropattern_script_';

    let API_BASE_URL;
    if (window.location.hostname === 'localhost' || window.location.hostname.startsWith("127")) {
        API_BASE_URL = 'http://localhost:8000';
    } else {
        API_BASE_URL = 'https://micropatterns-api.deno.dev';
    }
    
    const optimizationConfig = {
        enableTransformCaching: true,
        enablePatternTileCaching: true,
        enablePixelBatching: true,
        logOptimizationStats: false,
        logProfilingReport: false,
        enableOverdrawOptimization: false,
        enableLoopUnrolling: true,
        loopUnrollThreshold: 8,
        enableInvariantHoisting: true,
        enableFastPathSelection: true,
        enableSecondPassOptimization: true,
        enableDrawCallBatching: true,
        enableDeadCodeElimination: true,
        enableConstantFolding: true,
        enableTransformSequencing: true,
        enableDrawOrderOptimization: true,
        enableMemoryOptimization: true,
        enableOcclusionCulling: true,
        occlusionBlockSize: 16
    };

    let drawColor = 0;
    let lastDrawnPixel = { x: -1, y: -1 };

    const env = {
        HOUR: document.getElementById('envHour'),
        MINUTE: document.getElementById('envMinute'),
        SECOND: document.getElementById('envSecond'),
        COUNTER: document.getElementById('envCounter'),
        WIDTH: 540,
        HEIGHT: 960,
    };

    let m5PaperZoomFactor = 0.5;

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
        canvas.width = actualWidth;
        canvas.height = actualHeight;
        if (actualWidth === 540 && actualHeight === 960) {
            applyM5PaperZoom();
            zoomToggleButton.disabled = false;
        } else {
            canvas.style.width = actualWidth + 'px';
            canvas.style.height = actualHeight + 'px';
            zoomToggleButton.disabled = true;
            zoomToggleButton.textContent = "Zoom";
        }
        if (displayInfoSpan) {
            displayInfoSpan.textContent = `${actualWidth}x${actualHeight}`;
        }
    }

    const codeMirrorEditor = CodeMirror.fromTextArea(scriptInputTextArea, {
        lineNumbers: true, mode: "micropatterns", theme: "neat",
        indentUnit: 4, tabSize: 4, lineWrapping: true,
        extraKeys: { "Tab": "autocomplete" },
        hintOptions: { hint: micropatternsHint }
    });

    console.log("CodeMirror editor initialized:", codeMirrorEditor);

    function updateEditorTitle() {
        if (isViewMode || !editorTitleElement) return;
        const scriptName = scriptNameInput.value.trim();
        if (scriptName && currentScriptID) {
            editorTitleElement.textContent = `MICROPATTERNS ${scriptName}`;
        } else if (scriptName) {
            editorTitleElement.textContent = `MICROPATTERNS ${scriptName} (unsaved)`;
        } else {
            editorTitleElement.textContent = 'MICROPATTERNS SCRIPT';
        }
    }

    function saveContentToLocalStorage(scriptId, name, content, publishId, isPublished) {
        if (scriptId) {
            const scriptData = {
                id: scriptId, name: name, content: content,
                publishID: publishId,
                isPublished: isPublished,
                lastModified: new Date().toISOString()
            };
            localStorage.setItem(LS_SCRIPT_PREFIX + scriptId, JSON.stringify(scriptData));
            console.log(`Saved named script ${scriptId} to local storage:`, scriptData);
        } else {
            localStorage.setItem(LS_UNSAVED_CONTENT_KEY, content);
            if (name) localStorage.setItem(LS_UNSAVED_NAME_KEY, name); else localStorage.removeItem(LS_UNSAVED_NAME_KEY);
            console.log("Saved unnamed script content to local storage.");
        }
    }

    function updateUnsavedIndicator() {
        const indicator = document.getElementById('unsavedIndicator');
        if (indicator) {
            indicator.style.display = hasUnsavedChanges ? 'inline' : 'none';
        }
    }

    if (codeMirrorEditor) {
        codeMirrorEditor.on('change', () => {
            if (!isViewMode) {
                if (!currentScriptID) {
                    saveContentToLocalStorage(null, scriptNameInput.value, codeMirrorEditor.getValue(), null, false);
                }
                hasUnsavedChanges = true;
                updateUnsavedIndicator();
            }
        });
    }
    if (scriptNameInput) {
         scriptNameInput.addEventListener('input', () => {
            updateEditorTitle();
            if (!isViewMode) {
                if (!currentScriptID) {
                    saveContentToLocalStorage(null, scriptNameInput.value, codeMirrorEditor.getValue(), null, false);
                }
                hasUnsavedChanges = true;
                updateUnsavedIndicator();
            }
        });
    }

    function initializeUserId() {
        if (!userIdInput) {
            console.error("[Debug] User ID input field not found in initializeUserId.");
            updateScriptMgmtStatus("Error: User ID field missing in UI.", true);
            return;
        }
        const savedUserId = localStorage.getItem(LOCAL_STORAGE_USER_ID_KEY);
        if (savedUserId) {
            currentUserId = savedUserId;
        } else {
            currentUserId = generatePeerId ? generatePeerId() : "default-user-id";
            localStorage.setItem(LOCAL_STORAGE_USER_ID_KEY, currentUserId);
        }
        userIdInput.value = currentUserId;
        console.log('[Debug] Initializing User ID. Set to:', currentUserId);

        userIdInput.addEventListener('input', async () => { // Made async for await fetchScriptList
            const newUserId = userIdInput.value.trim();
            console.log('[Debug] User ID input changed. New input value:', newUserId);
            if (currentUserId === newUserId) return; // No change, do nothing

            currentUserId = newUserId;
            localStorage.setItem(LOCAL_STORAGE_USER_ID_KEY, currentUserId);
            console.log('[Debug] Current User ID updated to:', currentUserId);

            currentScriptID = null; currentPublishID = null; currentIsPublished = false; // Reset script context
            updatePublishControls(); updateEditorTitle(); updateUnsavedIndicator();
            // Clear script list dropdown before fetching new one
            populateScriptListDropdown([]);

            if (currentUserId) { // Only fetch if there's a user ID
                try {
                    await fetchScriptList(currentUserId); // Pass currentUserId
                } catch (error) {
                    console.error('[Debug] Error during fetchScriptList from userIdInput listener:', error);
                    updateScriptMgmtStatus(`Error loading scripts for ${currentUserId}: ${error.message || 'Unknown error'}.`, true);
                }
            } else {
                updateScriptMgmtStatus("Please enter a User ID to see scripts.", false);
            }
        });
    }

    async function fetchAPI(url, options = {}) { /* ... (existing code) ... */
        const defaultHeaders = { 'Content-Type': 'application/json', };
        const config = { ...options, headers: { ...defaultHeaders, ...(options.headers || {}), }, };
        try {
            const response = await fetch(url, config);
            if (!response.ok) {
                const errorData = await response.json().catch(() => ({ error: `HTTP error ${response.status}` }));
                throw new Error(errorData.error || `HTTP error ${response.status}`);
            }
            if (response.status === 204) return null;
            return response.json();
        } catch (error) {
            console.error('API Call Error:', error.message);
            throw error;
        }
    }

    function updatePublishControls() {
        if (!publishStatusContainer || isViewMode) return;
        publishStatusContainer.innerHTML = '';

        if (!currentUserId || !currentScriptID) {
            publishStatusContainer.innerHTML = '<p>Load or save a script to see publishing options.</p>';
            return;
        }

        const pStatus = document.createElement('p');
        let actionButton = document.createElement('button');

        if (currentPublishID) {
            if (currentIsPublished) {
                pStatus.textContent = 'Status: Published Live.';
                pStatus.style.color = 'green';
                publishStatusContainer.appendChild(pStatus);

                const openButton = document.createElement('button');
                openButton.textContent = '[Open]'; openButton.className = 'secondary'; openButton.style.marginRight = '5px';
                openButton.onclick = () => window.open(`?view=${currentPublishID}`, '_blank');
                publishStatusContainer.appendChild(openButton);

                const copyLinkButton = document.createElement('button');
                const fullViewUrlToCopy = new URL(`?view=${currentPublishID}`, window.location.href).href;
                copyLinkButton.textContent = 'Copy Link'; copyLinkButton.className = 'secondary'; copyLinkButton.style.marginRight = '5px';
                copyLinkButton.onclick = () => navigator.clipboard.writeText(fullViewUrlToCopy).then(() => setStatusMessage('Link copied!', false), err => setStatusMessage('Failed to copy link.', true));
                publishStatusContainer.appendChild(copyLinkButton);

                if (navigator.share) {
                    const shareButton = document.createElement('button');
                    shareButton.textContent = '[Share]'; shareButton.className = 'secondary'; shareButton.style.marginRight = '5px';
                    shareButton.onclick = async () => {
                        const scriptName = scriptNameInput.value.trim() || `Script ${currentScriptID}`;
                        try {
                            await navigator.share({ title: `Micropatterns: ${scriptName}`, text: `Check out this MicroPatterns script: ${scriptName}`, url: fullViewUrlToCopy });
                            setStatusMessage('Link shared!', false);
                        } catch (error) { if (error.name !== 'AbortError') setStatusMessage(`Share error: ${error.message}`, true); }
                    };
                    publishStatusContainer.appendChild(shareButton);
                }
                actionButton.textContent = 'Unpublish';
                actionButton.onclick = handleUnpublish;

            } else { // Has publishID but not live
                pStatus.innerHTML = `Status: Has Publish ID (<code>${currentPublishID}</code>), but not live.`;
                actionButton.textContent = '[Re-Publish]';
                actionButton.onclick = handlePublish;
                publishStatusContainer.appendChild(pStatus);
            }
        } else { // Never published
            pStatus.textContent = 'Status: Not Published.';
            actionButton.textContent = 'Publish';
            actionButton.onclick = handlePublish;
            publishStatusContainer.appendChild(pStatus);
        }
        actionButton.className = 'secondary';
        actionButton.style.marginTop = '5px';
        publishStatusContainer.appendChild(actionButton);
    }

    async function handlePublish() {
        if (!currentUserId || !currentScriptID) { setStatusMessage("User/Script ID missing.", true); return; }
        setStatusMessage("Publishing...", false);
        try {
            const responseData = await fetchAPI(`${API_BASE_URL}/api/scripts/${currentUserId}/${currentScriptID}/publish`, { method: 'POST' });
            if (responseData && responseData.success) {
                currentPublishID = responseData.publishID;
                currentIsPublished = responseData.isPublished;
                setStatusMessage("Script published!", false);
                updatePublishControls();
                if (responseData.script) {
                    saveContentToLocalStorage(currentScriptID, responseData.script.name, responseData.script.content, currentPublishID, currentIsPublished);
                } else { // Fallback if full script not in response
                     saveContentToLocalStorage(currentScriptID, scriptNameInput.value, codeMirrorEditor.getValue(), currentPublishID, currentIsPublished);
                }
            } else { throw new Error(responseData.error || "Publish failed."); }
        } catch (error) { setStatusMessage(`Error publishing: ${error.message}`, true); }
    }

    async function handleUnpublish() {
        if (!currentUserId || !currentScriptID || !currentPublishID) { setStatusMessage("Context incomplete or not published.", true); return; }
        setStatusMessage("Unpublishing...", false);
        try {
            const responseData = await fetchAPI(`${API_BASE_URL}/api/scripts/${currentUserId}/${currentScriptID}/unpublish`, { method: 'POST' });
            if (responseData && responseData.success) {
                currentIsPublished = responseData.isPublished; // Should be false
                setStatusMessage(`Script (ID: ${responseData.publishID}) unpublished.`, false);
                updatePublishControls();
                if (responseData.script) {
                     saveContentToLocalStorage(currentScriptID, responseData.script.name, responseData.script.content, currentPublishID, currentIsPublished);
                } else {
                     saveContentToLocalStorage(currentScriptID, scriptNameInput.value, codeMirrorEditor.getValue(), currentPublishID, currentIsPublished);
                }
            } else { throw new Error(responseData.error || "Unpublish failed."); }
        } catch (error) { setStatusMessage(`Error unpublishing: ${error.message}`, true); }
    }

    async function loadPublishedScriptForView(publishID) {
        setStatusMessage(`Loading published script ${publishID} for viewing...`, false);
        try {
            const scriptData = await fetchAPI(`${API_BASE_URL}/api/view/${publishID}`);
            // Check if scriptData is valid (fetchAPI returns null for 204, or valid JSON object)
            if (scriptData && scriptData.name && typeof scriptData.content === 'string') {
                setStatusMessage(`Published script '${scriptData.name}' loaded.`, false);
                return { script: scriptData, success: true };
            } else {
                // This case implies the API returned 200 OK but the data was not as expected,
                // or it was a 204 No Content, which is unexpected for a script view.
                console.warn("Published script data invalid or empty despite a 2xx API response.", scriptData);
                return { error: 'generic', success: false, message: "Script data received from server was invalid or empty." };
            }
        } catch (error) {
            // fetchAPI throws an error for non-ok responses; error.message includes status like "HTTP error 404"
            setStatusMessage(`Error loading published script: ${error.message}`, true); // Keep this for console logging
            let errorType = 'generic';
            // Check message content for 404 indication
            if (error.message && (error.message.includes('HTTP error 404') || error.message.toLowerCase().includes('not found'))) {
                errorType = 'not_found';
            }
            // The message property in the return will be used by displayNotFoundMessage if needed
            return { error: errorType, success: false, message: error.message };
        }
    }

    function displayNotFoundMessage(apiMessage = "") {
        const editorColumn = document.getElementById('editorColumn');
        const displayControlsColumn = document.getElementById('displayAndControlsColumn');
        const notFoundContainer = document.getElementById('viewScriptNotFoundMessageContainer');
        const editorTitle = document.getElementById('editorTitle');

        if (editorColumn) editorColumn.style.display = 'none';
        if (displayControlsColumn) displayControlsColumn.style.display = 'none';

        // Hide other specific view mode elements that might have been made visible
        const runButtonElement = document.getElementById('runButton');
        if (runButtonElement) runButtonElement.style.display = 'none';
        const publishControlsElement = document.getElementById('publishControls'); // This holds "Copy & Edit"
        if (publishControlsElement) publishControlsElement.style.display = 'none';

        // Also hide elements that initial view mode setup might attempt to hide,
        // or that are part of the general editor UI that shouldn't show on a "not found" page.
        if (document.getElementById('scriptManagementControls')) document.getElementById('scriptManagementControls').style.display = 'none';
        if (document.getElementById('optimizationSettings')) document.getElementById('optimizationSettings').style.display = 'none';
        if (document.getElementById('environmentControls')) document.getElementById('environmentControls').style.display = 'none';
        if (document.getElementById('deviceSyncControls')) document.getElementById('deviceSyncControls').style.display = 'none';
        const editorControlsElement = document.getElementById('editorControls'); // Theme select, line wrap etc.
        if (editorControlsElement) editorControlsElement.style.display = 'none';


        if (editorTitle) editorTitle.textContent = 'Script Not Found';
        document.title = 'Script Not Found - MicroPatterns';

        if (notFoundContainer) {
            let messageDetail = "<p style='color: #555; font-size: 16px; margin: 15px 0;'>The script you are trying to view either does not exist or is no longer published.</p>";
            // Example of how apiMessage could be used for debugging, keeping user message simple:
            // if (apiMessage) {
            //     messageDetail = `<p style='color: #777; font-size: 12px; font-style: italic;'>Detail: ${apiMessage}</p>` + messageDetail;
            // }
            notFoundContainer.innerHTML = `
                <div style="text-align: center; padding: 40px 20px; border: 1px solid #ddd; margin: 50px auto; max-width: 600px; background-color: #f9f9f9; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1);">
                    <h2 style="color: #333; font-size: 24px;">Script Not Available</h2>
                    ${messageDetail}
                    <p style="color: #555; font-size: 16px; margin-bottom: 25px;">Why not create your own masterpiece?</p>
                    <button id="createOwnScriptButtonViewNotFound" style="background-color: #007bff; color: white; padding: 12px 25px; border: none; border-radius: 5px; font-size: 16px; cursor: pointer;">Create Your Own Script</button>
                </div>
            `;
            notFoundContainer.style.display = 'block'; // Make sure it's visible

            const createButton = document.getElementById('createOwnScriptButtonViewNotFound');
            if (createButton) {
                createButton.addEventListener('click', () => {
                    window.location.href = 'index.html';
                });
            }
        }
    }

    function setupViewModeUI(scriptData) {
        // Note: scriptData is now guaranteed to be valid if this function is called.

        // Ensure main columns are visible and notFoundContainer is hidden when script IS found.
        const editorColumn = document.getElementById('editorColumn');
        const displayControlsColumn = document.getElementById('displayAndControlsColumn');
        if (editorColumn) editorColumn.style.display = ''; // Reset to default CSS display (e.g., flex)
        if (displayControlsColumn) displayControlsColumn.style.display = ''; // Reset to default CSS display

        const notFoundContainer = document.getElementById('viewScriptNotFoundMessageContainer');
        if (notFoundContainer) notFoundContainer.style.display = 'none';

        document.title = `View Script - ${scriptData.name}`;
        if (editorTitleElement) editorTitleElement.textContent = `VIEWING: ${scriptData.name}`;
        codeMirrorEditor.setValue(scriptData.content);
        codeMirrorEditor.setOption("readOnly", true);
        document.querySelectorAll('.controls-group').forEach(group => {
            const h3Text = group.querySelector('h3')?.textContent.trim();
            if (h3Text === "Script Management" || h3Text === "Execution Path & Optimizations" || h3Text === "Device Sync Scripts" || h3Text === "Environment") {
                 group.style.display = 'none';
            }
        });
        document.querySelectorAll('.inline-flex-center').forEach(el => el.style.display = 'none');
        if (publishStatusContainer) {
            publishStatusContainer.innerHTML = '';
            const copyEditButton = document.createElement('button');
            copyEditButton.textContent = 'Copy and Edit This Script';
            copyEditButton.className = 'primary';
            copyEditButton.style.width = '100%'; copyEditButton.style.padding = '10px'; copyEditButton.style.fontSize = '1.1em';
            copyEditButton.addEventListener('click', () => {
                sessionStorage.setItem('copiedScriptName', scriptData.name);
                sessionStorage.setItem('copiedScriptContent', scriptData.content);
                window.location.href = window.location.pathname;
            });
            publishStatusContainer.appendChild(copyEditButton);
            if (document.getElementById('publishControls')) {
                 document.getElementById('publishControls').style.display = 'block';
            }
        }
        runScript();
    }
    if (lineWrapToggle && codeMirrorEditor) { /* ... (existing code) ... */ }
    function applyTheme(themeFile) { /* ... (existing code) ... */ }
    if (themeStylesheet && themeSelect) {  /* ... (existing code) ... */ }
    function updateOptimizationVisibility() { /* ... (existing code, but setupOptimizationUI should be guarded) ... */ }
    function setupOptimizationUI() { /* ... (existing code) ... */ }
    function updateSecondPassDependentOptionsUI() { /* ... (existing code) ... */ }

    // Guard optimization UI setup for non-view mode
    if (!isViewMode) {
        const executionPathRadios = document.querySelectorAll('input[name="executionPath"]');
        executionPathRadios.forEach(radio => {
            if (radio.value === executionPath) radio.checked = true;
            radio.addEventListener('change', function(e) {
                if (e.target.checked) { executionPath = e.target.value; updateOptimizationVisibility(); }
            });
        });
        checkboxesConfig.forEach(cbConfig => {
            const checkbox = document.getElementById(cbConfig.id);
            if (checkbox) {
                checkbox.checked = optimizationConfig[cbConfig.configKey];
                checkbox.addEventListener('change', function(e) {
                    optimizationConfig[cbConfig.configKey] = e.target.checked;
                    if (cbConfig.id === 'enableSecondPassOptimization') updateSecondPassDependentOptionsUI();
                });
            }
        });
        updateOptimizationVisibility();
    }


    const micropatternsKeywords = [ "DEFINE", "PATTERN", "VAR", "LET", "COLOR", "FILL", "DRAW", "RESET_TRANSFORMS", "TRANSLATE", "ROTATE", "SCALE", "PIXEL", "LINE", "RECT", "FILL_RECT", "CIRCLE", "FILL_CIRCLE", "REPEAT", "TIMES", "IF", "THEN", "ELSE", "ENDIF", "ENDREPEAT", "NAME=", "WIDTH=", "HEIGHT=", "DATA=", "X=", "Y=", "X1=", "Y1=", "X2=", "Y2=", "DX=", "DY=", "DEGREES=", "FACTOR=", "RADIUS=", "COUNT=", "BLACK", "WHITE", "SOLID" ];
    const micropatternsEnvVars = [ "$HOUR", "$MINUTE", "$SECOND", "$COUNTER", "$WIDTH", "$HEIGHT", "$INDEX" ];
    function getUserDefinedVars(editor) { /* ... (existing code) ... */ return []; }
    function micropatternsHint(editor) { /* ... (existing code) ... */ return null; }

    function updateRealTimeDisplay() { /* ... (existing code) ... */ }
    updateRealTimeDisplay(); setInterval(updateRealTimeDisplay, 1000);

    if (displaySizeSelect) { /* ... (existing code for display size and zoom) ... */ }
    if (zoomToggleButton) { /* ... (existing code for zoom) ... */ }

    function getEnvironmentVariables() { /* ... (existing code) ... */ return {};}
    function displayError(message, type = "Error") { /* ... (existing code) ... */ }
    function clearDisplay() { /* ... (existing code) ... */ }
    function runScript() { /* ... (existing code) ... */ }

    if(runButton){ // Always add run button listener, setupViewModeUI might hide it.
        runButton.addEventListener('click', runScript);
    }
    if (isViewMode && runButton) runButton.style.display = 'none'; // Explicitly hide if not covered


    if (lockCounterButton && env.COUNTER) { /* ... (existing code) ... */ }
    if (resetCounterButton && env.COUNTER) { /* ... (existing code) ... */ }

    const PREVIEW_SCALE = 12;
    function renderAssetPreviews(assets) { /* ... (existing code) ... */ }
    function renderSingleAssetPreview(asset) { /* ... (existing code, already checks isViewMode) ... */ }
    function handleDragOver(event) { /* ... (existing code) ... */ }
    function handleDragLeave(event) { /* ... (existing code) ... */ }
    function handleDrop(event, targetCanvas, targetAsset) { /* ... (existing code) ... */ }
    function drawAssetOnCanvas(ctx, asset, scale) { /* ... (existing code) ... */ }
    function drawSinglePixelOnPreview(ctx, x, y, colorValue, scale) { /* ... (existing code) ... */ }
    function handleMouseDown(event, canvas, asset) { /* ... (existing code) ... */ }
    function handleMouseMove(event, canvas, asset) { /* ... (existing code) ... */ }
    function handleMouseUpOrLeave(canvas, asset) { /* ... (existing code) ... */ }
    function updateCodeMirrorAssetData(assetType, assetNameUpper, newPixelData) { /* ... (existing code) ... */ }

    function resetEnvironmentInputs() { /* ... (existing code) ... */ }

    function updateScriptMgmtStatus(message, isError = false) {
        const statusEl = document.getElementById('scriptMgmtStatus');
        if (statusEl) {
            statusEl.textContent = message;
            statusEl.style.color = isError ? 'red' : '#006400'; // Dark green for success, red for error
            console.log(`[ScriptMgmtStatus] ${isError ? 'ERROR':'INFO'}: ${message}`);
        } else {
            // Fallback to general console log if specific element isn't found
            const logMethod = isError ? console.error : console.log;
            logMethod(`[ScriptMgmtStatus - Element not found] ${message}`);
        }
    }

    // Note: setStatusMessage is being replaced by updateScriptMgmtStatus.
    // If setStatusMessage was used elsewhere for different purposes, that needs to be handled.
    // For now, assuming it was primarily for scriptMgmtStatus.
    // function setStatusMessage(message, isError = false) { /* ... (existing code) ... */ }


    let scriptsCache = []; // Cache for the fetched scripts

    async function fetchScriptList(userIdToFetch) {
        console.log('[Debug] fetchScriptList called with User ID:', userIdToFetch);
        if (!userIdToFetch) {
            console.warn('[Debug] fetchScriptList called with no User ID.');
            populateScriptListDropdown([]); // Clear dropdown
            updateScriptMgmtStatus('Please enter a User ID to load scripts.', false); // Not an error, but an instruction
            return;
        }

        updateScriptMgmtStatus(`Fetching scripts for ${userIdToFetch}...`, false);
        const fetchUrl = `${API_BASE_URL}/api/scripts/${userIdToFetch}`;
        console.log('[Debug] Fetching script list for User ID:', userIdToFetch, 'from URL:', fetchUrl);

        try {
            const responseData = await fetchAPI(fetchUrl); // fetchAPI throws on network/HTTP errors

            // If fetchAPI resolves, it means HTTP status was OK.
            // Now, validate the structure of responseData.
            if (!Array.isArray(responseData)) {
                console.error('[Debug] Script list data is not an array:', responseData);
                updateScriptMgmtStatus('Error: Unexpected format for script list from server.', true);
                populateScriptListDropdown([]);
                return;
            }

            console.log('[Debug] Successfully fetched script list data:', responseData);
            scriptsCache = responseData;
            populateScriptListDropdown(scriptsCache);
            updateScriptMgmtStatus(scriptsCache.length > 0 ? `Scripts loaded for ${userIdToFetch}.` : `No scripts found for ${userIdToFetch}.`, false);

        } catch (error) {
            // This catch block handles errors thrown by fetchAPI (network errors, HTTP non-2xx errors)
            console.error('[Debug] Error in fetchScriptList > fetchAPI call for User ID', userIdToFetch, ':', error);
            // error.message from fetchAPI should contain "HTTP error XXX" or a network error message.
            updateScriptMgmtStatus(`Failed to load script list: ${error.message || 'Network or server error'}.`, true);
            populateScriptListDropdown([]); // Clear dropdown on error
        }
    }

    // Renamed from populateDeviceScriptList to avoid confusion, this is for the main script load dropdown
    function populateScriptListDropdown(scripts) {
        console.log('[Debug] populateScriptListDropdown called with scripts:', scripts);
        const scriptListDropdown = document.getElementById('scriptList');
        if (!scriptListDropdown) {
            console.error('[Debug] scriptListDropdown element not found!');
            return;
        }

        scriptListDropdown.innerHTML = '<option value="">-- Select Script --</option>'; // Clear existing options, add default

        if (Array.isArray(scripts)) {
            scripts.forEach(script => {
                if (script && script.id && script.name) { // Basic validation
                    const option = document.createElement('option');
                    option.value = script.id;
                    option.textContent = script.name;
                    scriptListDropdown.appendChild(option);
                } else {
                    console.warn('[Debug] Invalid script item found in list:', script);
                }
            });
        } else {
            console.error('[Debug] populateScriptListDropdown received non-array scripts argument:', scripts);
            updateScriptMgmtStatus("Error: Could not populate script list due to invalid data format.", true);
        }
    }


    function populateDeviceScriptList(allScripts, deviceScriptIds) { /* ... (existing code) ... */ }
    async function updateDeviceSelection() { /* ... (existing code) ... */ }

    async function loadScript(scriptId) {
        if (!currentUserId || !scriptId) { setStatusMessage("User/Script ID missing.", true); return; }
        setStatusMessage(`Loading script '${scriptId}'...`);
        resetEnvironmentInputs();
        try {
            const scriptDataFromServer = await fetchAPI(`${API_BASE_URL}/api/scripts/${currentUserId}/${scriptId}`);
            if (!scriptDataFromServer) throw new Error("Script not found or empty response.");

            scriptNameInput.value = scriptDataFromServer.name || '';
            codeMirrorEditor.setValue(scriptDataFromServer.content || '');
            currentScriptID = scriptId;
            currentPublishID = scriptDataFromServer.publishID || null;
            currentIsPublished = scriptDataFromServer.isPublished === true;

            setStatusMessage(`Script '${scriptDataFromServer.name}' loaded.`, false);
            hasUnsavedChanges = false;
            updateUnsavedIndicator(); updateEditorTitle(); updatePublishControls();
            saveContentToLocalStorage(currentScriptID, scriptDataFromServer.name, scriptDataFromServer.content, currentPublishID, currentIsPublished);
            history.replaceState(null, '', '#scriptID=' + currentScriptID);
            runScript();
        } catch (error) {
            setStatusMessage(`Error loading script: ${error.message}`, true);
            currentScriptID = null; currentPublishID = null; currentIsPublished = false;
            hasUnsavedChanges = false; updateUnsavedIndicator(); updateEditorTitle(); updatePublishControls();
        }
    }

    async function saveScript() {
        if (!currentUserId) { setStatusMessage("User ID missing.", true); return; }
        const scriptName = scriptNameInput.value.trim();
        if (!scriptName) { setStatusMessage("Script name required.", true); return; }
        const scriptContent = codeMirrorEditor.getValue();
        const generatedScriptId = scriptName.toLowerCase().replace(/\s+/g, '-').replace(/[^a-z0-9-]/g, '').substring(0, 50);
        if (!generatedScriptId) { setStatusMessage("Invalid script name for ID.", true); return; }

        let scriptIdToSave = generatedScriptId;
        // If currentScriptID exists and the name hasn't changed from what's selected, it's an update to existing.
        // Otherwise, it's a new script or "Save As" new name.
        if (currentScriptID) {
            const selectedOption = scriptListSelect.options[scriptListSelect.selectedIndex];
            if (selectedOption && selectedOption.value === currentScriptID && scriptNameInput.value === selectedOption.text) {
                scriptIdToSave = currentScriptID; // Updating existing script
            } else { // Name changed or was a new script with a name; treat as new for publish state
                currentScriptID = generatedScriptId; // New ID for this save
                currentPublishID = null;
                currentIsPublished = false;
            }
        } else { // Saving a brand new script (no currentScriptID yet)
             currentScriptID = generatedScriptId;
             currentPublishID = null;
             currentIsPublished = false;
        }

        setStatusMessage(`Saving '${scriptName}'...`);
        const payload = { name: scriptName, content: scriptContent, publishID: currentPublishID, isPublished: currentIsPublished };
        try {
            const responseData = await fetchAPI(`${API_BASE_URL}/api/scripts/${currentUserId}/${scriptIdToSave}`, { method: 'PUT', body: JSON.stringify(payload) });
            const savedData = responseData.script;

            currentScriptID = savedData.id;
            currentPublishID = savedData.publishID || null;
            currentIsPublished = savedData.isPublished === true;
            scriptNameInput.value = savedData.name;

            saveContentToLocalStorage(currentScriptID, savedData.name, savedData.content, currentPublishID, currentIsPublished);
            history.replaceState(null, '', '#scriptID=' + currentScriptID);

            hasUnsavedChanges = false;
            updateUnsavedIndicator(); updateEditorTitle(); updatePublishControls();
            setStatusMessage(`Script '${savedData.name}' saved.`, false);
            await fetchScriptList();
            if (Array.from(scriptListSelect.options).find(opt => opt.value === currentScriptID)) {
                 scriptListSelect.value = currentScriptID;
            }
        } catch (error) { setStatusMessage(`Error saving: ${error.message}`, true); updatePublishControls(); }
    }

    function createConfirmationDialog(message, options) { /* ... (existing code) ... */ return Promise.resolve("discard");}

    async function newScript() {
        const currentContent = codeMirrorEditor.getValue().trim();
        const defaultContent = newScriptTemplateContent().trim();
        if (currentContent === '' || currentContent === defaultContent || !hasUnsavedChanges) {
            createNewScript(); return;
        }
        const result = await createConfirmationDialog("Discard unsaved changes and create a new script?", [
            { label: "Discard & New", value: "discard", destructive: true },
            { label: "Cancel", value: "cancel" }
        ]);
        if (result === "discard") createNewScript();
    }

    function createNewScript() {
        scriptNameInput.value = '';
        codeMirrorEditor.setValue(newScriptTemplateContent());
        scriptListSelect.value = '';
        currentScriptID = null; currentPublishID = null; currentIsPublished = false;
        localStorage.removeItem(LS_UNSAVED_NAME_KEY);
        saveContentToLocalStorage(null, '', codeMirrorEditor.getValue(), null, false);
        history.replaceState(null, '', window.location.pathname + window.location.search);
        hasUnsavedChanges = false;
        updateUnsavedIndicator(); updateEditorTitle();
        setStatusMessage("New script created.", false);
        updatePublishControls();
        runScript();
    }

    if (loadScriptButton && !isViewMode) loadScriptButton.addEventListener('click', () => loadScript(scriptListSelect.value));
    if (saveScriptButton && !isViewMode) saveScriptButton.addEventListener('click', saveScript);
    if (newScriptButton && !isViewMode) newScriptButton.addEventListener('click', newScript);


    function displayProfilingResults() { /* ... (existing code) ... */ }
    function wrapForProfiling(instance, methodName, dataStore) { /* ... (existing code) ... */ }

    // --- Initial Load ---
    if (isViewMode && viewPublishID) {
        console.log("View Mode Detected. Publish ID:", viewPublishID);

        // Initial hiding of main content columns to prevent flash during load.
        const editorCol = document.getElementById('editorColumn');
        const displayCol = document.getElementById('displayAndControlsColumn');
        if (editorCol) editorCol.style.display = 'none';
        if (displayCol) displayCol.style.display = 'none';

        // Minimal UI setup that might always apply in view mode (theme, display size, zoom).
        // These are less critical if the page ends up being "Not Found".
        if (themeSelect) { /* Minimal theme setup if any needed before load attempt */ }
        if (displaySizeSelect) { /* Minimal display setup if any */ }
        if (zoomToggleButton) { /* Minimal zoom setup if any */ }

        // Hide control groups that are definitely not needed in any view mode scenario early.
        // displayNotFoundMessage and setupViewModeUI also manage some of these, this is an early measure.
        const editorControlsElement = document.getElementById('editorControls'); // Theme select, line wrap etc.
        if (editorControlsElement) editorControlsElement.style.display = 'none';
        // Other major control groups that are editor-specific:
        if (document.getElementById('scriptManagementControls')) document.getElementById('scriptManagementControls').style.display = 'none';
        if (document.getElementById('optimizationSettings')) document.getElementById('optimizationSettings').style.display = 'none';
        if (document.getElementById('environmentControls')) document.getElementById('environmentControls').style.display = 'none';
        if (document.getElementById('deviceSyncControls')) document.getElementById('deviceSyncControls').style.display = 'none';

        const publishIdRegex = /^[a-zA-Z0-9-_]{21}$/;
        if (!publishIdRegex.test(viewPublishID)) {
            console.warn('Invalid Publish ID format:', viewPublishID);
            displayNotFoundMessage('Invalid script ID format.');
            // Since DOMContentLoaded is async, ensure no further script loading attempts for view mode.
            // The rest of the function (normal mode setup) should not run if isViewMode is true.
        } else {
            const loadResult = await loadPublishedScriptForView(viewPublishID);
            if (loadResult.success) {
                // setupViewModeUI will make editorCol and displayCol visible.
                setupViewModeUI(loadResult.script);
            } else {
                // loadResult.message contains the error message from fetchAPI or load function.
                displayNotFoundMessage(loadResult.message);
            }
        }
    } else { // Normal Editor Mode
        console.log("Normal Editor Mode");
        // Ensure columns are visible for normal editor mode, in case they were hidden by prior view mode logic
        // (e.g. if isViewMode was true but regex failed, then somehow this block was reached - robust).
        const editorCol = document.getElementById('editorColumn');
        const displayCol = document.getElementById('displayAndControlsColumn');
        if (editorCol) editorCol.style.display = ''; // Reset to CSS default
        if (displayCol) displayCol.style.display = ''; // Reset to CSS default
        let scriptLoadedFromSessionOrHash = false;
        const copiedName = sessionStorage.getItem('copiedScriptName');
        const copiedContent = sessionStorage.getItem('copiedScriptContent');

        if (copiedName !== null && copiedContent !== null) {
            scriptNameInput.value = copiedName; codeMirrorEditor.setValue(copiedContent);
            currentScriptID = null; currentPublishID = null; currentIsPublished = false;
            sessionStorage.removeItem('copiedScriptName'); sessionStorage.removeItem('copiedScriptContent');
            setStatusMessage("Script copied for editing.", false);
            hasUnsavedChanges = true;
            scriptLoadedFromSessionOrHash = true;
        } else if (window.location.hash && window.location.hash.startsWith('#scriptID=')) {
            const scriptIdFromHash = window.location.hash.substring('#scriptID='.length);
            const storedScriptJSON = localStorage.getItem(LS_SCRIPT_PREFIX + scriptIdFromHash);
            if (storedScriptJSON) {
                try {
                    const storedScript = JSON.parse(storedScriptJSON);
                    scriptNameInput.value = storedScript.name || '';
                    codeMirrorEditor.setValue(storedScript.content || '');
                    currentScriptID = storedScript.id;
                    currentPublishID = storedScript.publishID || null;
                    currentIsPublished = storedScript.isPublished === true;
                    setStatusMessage(`Loaded '${storedScript.name}' via URL.`, false);
                    hasUnsavedChanges = false;
                    scriptLoadedFromSessionOrHash = true;
                } catch (e) { history.replaceState(null, '', window.location.pathname + window.location.search); }
            } else { history.replaceState(null, '', window.location.pathname + window.location.search); }
        }

        if (!scriptLoadedFromSessionOrHash) {
            const unsavedContent = localStorage.getItem(LS_UNSAVED_CONTENT_KEY);
            const unsavedName = localStorage.getItem(LS_UNSAVED_NAME_KEY);
            if (unsavedContent !== null) {
                codeMirrorEditor.setValue(unsavedContent);
                if (unsavedName !== null) scriptNameInput.value = unsavedName;
                hasUnsavedChanges = codeMirrorEditor.getValue() !== newScriptTemplateContent();
            } else {
                 codeMirrorEditor.setValue(newScriptTemplateContent());
                 hasUnsavedChanges = false;
                 currentPublishID = null; currentIsPublished = false;
            }
        }

        initializeUserId();
        if (!isViewMode) { // Full UI setup only if not in view mode
            const theme = localStorage.getItem(LOCAL_STORAGE_THEME_KEY) || 'style.css'; applyTheme(theme);
            if(themeSelect) themeSelect.addEventListener('change', (e) => applyTheme(e.target.value));

            const dsVal = displaySizeSelect.value; const [dsW, dsH] = dsVal.split('x').map(Number);
            m5PaperZoomFactor=0.5; updateCanvasDimensions(dsW,dsH);
            if(displaySizeSelect) displaySizeSelect.addEventListener('change', (e)=>{ const [w,h]=e.target.value.split('x').map(Number); if(w===540 && h===960)m5PaperZoomFactor=0.5; updateCanvasDimensions(w,h);runScript(); });
            if(zoomToggleButton) zoomToggleButton.addEventListener('click', ()=>{if(canvas.width===540 && canvas.height===960){m5PaperZoomFactor=(m5PaperZoomFactor===0.5)?1.0:0.5;applyM5PaperZoom();}});

            setupOptimizationUI();
            // Initial fetch of script list using the determined currentUserId
            if (currentUserId) { // Only fetch if userId is available
                try {
                    console.log(`[Debug] Initial fetchScriptList with User ID: ${currentUserId} from DOMContentLoaded`);
                    await fetchScriptList(currentUserId);
                } catch (error) {
                    console.error('[Debug] Error during initial fetchScriptList from DOMContentLoaded:', error);
                    updateScriptMgmtStatus(`Failed to load initial script list: ${error.message || 'Unknown error'}.`, true);
                }
            } else {
                console.log("[Debug] No User ID available on initial load, skipping fetchScriptList.");
                updateScriptMgmtStatus("Enter User ID to load scripts.", false);
            }

            if (currentScriptID && scriptListSelect.querySelector(`option[value="${currentScriptID}"]`)) {
                scriptListSelect.value = currentScriptID;
            }
        }
        updateEditorTitle();
        updatePublishControls();
        updateUnsavedIndicator();
        runScript();
    }
});

function newScriptTemplateContent() {
    const w = (typeof env !== 'undefined' && env.WIDTH) ? env.WIDTH : 540;
    const h = (typeof env !== 'undefined' && env.HEIGHT) ? env.HEIGHT : 960;
    return `# New MicroPatterns Script\n# Display is ${w}x${h}

DEFINE PATTERN NAME="stripes" WIDTH=8 HEIGHT=8 DATA="1111111100000000111111110000000011111111000000001111111100000000"

VAR $CENTERX = $WIDTH / 2
VAR $CENTERY = $HEIGHT / 2
VAR $SECONDPLUSONE = $SECOND + 1

COLOR NAME=BLACK
FILL NAME="stripes"
FILL_RECT X=0 Y=0 WIDTH=$WIDTH HEIGHT=$HEIGHT
`;
}