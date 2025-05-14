/**
 * Scroll Handler for MicroPatterns Emulator v3
 *
 * Manages scrolling states: 'normal', 'fixed' (CodeMirror sticks to top), 'bottom-aligned'.
 * Disables itself in mobile view (<= 1200px).
 * Aims for smooth transitions and accurate positioning, especially ensuring the error log is fully visible at the bottom.
 */
document.addEventListener('DOMContentLoaded', () => {
    // --- Elements ---
    const columns = document.querySelectorAll('body > .column');
    const editorColumn = columns[0] || null;
    const toolsColumn = columns[1] || null;
    const codeMirrorEl = document.querySelector('.CodeMirror');
    const errorLogEl = document.getElementById('errorLog');

    // --- Verification ---
    if (!editorColumn || !toolsColumn || !codeMirrorEl) {
        console.warn("Scroll handler: Required elements not found (editorColumn, toolsColumn, or .CodeMirror).");
        return;
    }

    // --- Configuration ---
    const CONFIG = {
        HYSTERESIS_UP: 25,          // Pixels resistance scrolling up
        HYSTERESIS_DOWN: 25,        // Pixels resistance scrolling down
        MIN_CHANGE_INTERVAL: 150,   // Min ms between state changes to prevent rapid flicker
        TRANSITION_DURATION: 50,    // Estimated transition time (ms) for state change logic
        MOBILE_BREAKPOINT: 1200,    // Width threshold for mobile view (px)
        ERROR_CHECK_INTERVAL: 500,  // Interval to check error log height (ms)
        ERROR_HEIGHT_TOLERANCE: 5   // Min pixel change in error log height to trigger update
    };

    // --- State Variables ---
    let metrics = null;                 // Cached measurements { bodyPaddingTop, columnGap, editorWidth, editorTop, editorHeight, cmTop, cmOffsetFromColumn, viewportHeight, documentHeight, maxScrollY, fixThreshold, bottomThreshold, fixedTopVal }
    let currentState = 'normal';        // 'normal', 'fixed', 'bottom-aligned'
    let lastScrollY = 0;                // Previous scroll position
    let lastStateChangeTime = 0;        // Timestamp of last state change
    let scrollDirection = 'none';       // 'up', 'down', 'none'
    let isTransitioning = false;        // Flag to prevent overlapping transitions
    let ignoreMutation = false;         // Flag to ignore self-triggered mutations
    let isMobile = isMobileView();      // Track mobile state
    let lastErrorLogHeight = 0;         // Track error log height

    // --- Core Functions ---

    /** Checks if the window width is below the mobile breakpoint. */
    function isMobileView() {
        return window.innerWidth <= CONFIG.MOBILE_BREAKPOINT;
    }

    /** Calculates and caches geometric metrics needed for scroll logic. */
    function calculateMetrics() {
        // Temporarily reset styles for accurate measurement
        const oldStyles = {
            editorPos: editorColumn.style.position, editorTop: editorColumn.style.top, editorWidth: editorColumn.style.width, editorZ: editorColumn.style.zIndex,
            toolsMargin: toolsColumn.style.marginLeft
        };
        resetPositioningStyles(); // Apply default styles

        const bodyStyle = window.getComputedStyle(document.body);
        const bodyPaddingTop = parseInt(bodyStyle.paddingTop) || 0;
        const columnGap = parseInt(bodyStyle.gap) || 0;

        const editorRect = editorColumn.getBoundingClientRect();
        const editorWidth = editorRect.width;
        const editorTop = window.scrollY + editorRect.top; // Absolute top relative to document
        const editorHeight = editorColumn.offsetHeight; // Includes padding, border, content, and handles errorLog height
        const editorBottom = editorTop + editorHeight;

        const cmRect = codeMirrorEl.getBoundingClientRect();
        const cmTop = window.scrollY + cmRect.top; // Absolute top relative to document
        const cmOffsetFromColumn = cmTop - editorTop; // Vertical distance from editor top to CodeMirror top

        const viewportHeight = window.innerHeight;
        const documentHeight = Math.max(
            document.body.scrollHeight, document.documentElement.scrollHeight,
            document.body.offsetHeight, document.documentElement.offsetHeight,
            document.body.clientHeight, document.documentElement.clientHeight
        );
        const maxScrollY = Math.max(0, documentHeight - viewportHeight);

        // --- Threshold Calculations ---
        // 1. fixThreshold: ScrollY when CodeMirror top reaches body padding top
        const fixThreshold = Math.max(0, cmTop - bodyPaddingTop);

        // 2. fixedTopVal: The 'top' value when the editor is fixed
        const fixedTopVal = bodyPaddingTop - cmOffsetFromColumn;

        // 3. bottomThreshold: ScrollY when the *natural bottom* of the editor column reaches the viewport bottom.
        //    This determines when the column should stop scrolling naturally and become bottom-aligned.
        let bottomThreshold = Math.max(0, editorBottom - viewportHeight);
        // Ensure bottomThreshold isn't triggered before the element is fixed.
        // If the editor is very tall, bottomThreshold might be less than fixThreshold.
        // In this case, it should transition directly from normal to bottom-aligned if needed,
        // but for simplicity and expected layout, we usually fix first.
        // Let's ensure it doesn't switch *back* from fixed to normal *before* bottom-aligned is needed.
        // If bottomThreshold < fixThreshold, it implies the bottom is visible before the top needs fixing.
        // The state logic should handle this, but let's keep the calculation direct for now.
        // bottomThreshold = Math.max(fixThreshold, bottomThreshold); // Re-evaluate if this constraint is needed.

        // 4. bottomAlignedTopVal: The constant 'top' value when the editor is absolutely positioned at the bottom.
        const bottomAlignedTopVal = Math.max(0, documentHeight - editorHeight);


        // Restore original styles if they existed (might be needed if called mid-state)
        // Note: This might cause a flicker if called frequently. Consider if needed.
        // applyStyles(oldStyles.editorPos, oldStyles.editorTop, oldStyles.editorWidth, oldStyles.editorZ, oldStyles.toolsMargin);

        // Update last error log height used for change detection
        if (errorLogEl) lastErrorLogHeight = errorLogEl.offsetHeight;

        return {
            bodyPaddingTop, columnGap, editorWidth, editorTop, editorHeight, editorBottom,
            cmTop, cmOffsetFromColumn, viewportHeight, documentHeight, maxScrollY,
            fixThreshold, bottomThreshold, fixedTopVal, bottomAlignedTopVal // Added bottomAlignedTopVal
        };
    }

    /** Resets element styles to their default (non-fixed, non-absolute) state. */
    function resetPositioningStyles() {
        applyStyles('', '', '', '', '');
    }

    /** Applies positioning styles, checking if changes are needed to prevent flicker/loops. */
    function applyStyles(editorPos, editorTop, editorWidth, editorZ, toolsMargin) {
        ignoreMutation = true; // Set flag before changing styles

        let changed = false;
        if (editorColumn.style.position !== editorPos) { editorColumn.style.position = editorPos; changed = true; }
        if (editorColumn.style.top !== editorTop) { editorColumn.style.top = editorTop; changed = true; }
        if (editorColumn.style.width !== editorWidth) { editorColumn.style.width = editorWidth; changed = true; }
        if (editorColumn.style.zIndex !== editorZ) { editorColumn.style.zIndex = editorZ; changed = true; }
        if (toolsColumn.style.marginLeft !== toolsMargin) { toolsColumn.style.marginLeft = toolsMargin; changed = true; }

        // Request animation frame to reset the flag *after* the browser has processed the style changes
        // This helps prevent the MutationObserver from reacting to these changes.
        requestAnimationFrame(() => {
            ignoreMutation = false;
        });

        // Refresh CodeMirror only if styles actually changed
        if (changed) {
            refreshCodeMirror();
        }
    }

    /** Refreshes CodeMirror instance if it exists. */
    function refreshCodeMirror() {
        if (codeMirrorEl && codeMirrorEl.CodeMirror) {
            try {
                codeMirrorEl.CodeMirror.refresh();
            } catch (e) {
                console.warn("Error refreshing CodeMirror:", e);
            }
        }
    }

    /** Applies styles for the 'normal' scrolling state. */
    function applyNormalState() {
        resetPositioningStyles();
    }

    /** Applies styles for the 'fixed' scrolling state. */
    function applyFixedState() {
        if (!metrics) return;
        const topPx = `${metrics.fixedTopVal}px`;
        const widthPx = `${metrics.editorWidth}px`;
        const marginPx = `${metrics.editorWidth + metrics.columnGap}px`;
        applyStyles('fixed', topPx, widthPx, '1', marginPx);
    }

    /** Applies styles for the 'bottom-aligned' state using pre-calculated top value. */
    function applyBottomAlignedState() { // Removed scrollY parameter
        if (!metrics) return;
        // Use the pre-calculated top value that aligns the element's bottom with the document bottom.
        const topPx = `${metrics.bottomAlignedTopVal}px`;
        const widthPx = `${metrics.editorWidth}px`;
        const marginPx = `${metrics.editorWidth + metrics.columnGap}px`;
        applyStyles('absolute', topPx, widthPx, '', marginPx); // z-index not needed for absolute
    }

    /** Determines the target scroll state based on scrollY and hysteresis. */
    function getTargetState(scrollY) {
        if (!metrics) return 'normal';

        // Clamp scrollY for state calculation, but not for positioning
        const clampedScrollY = Math.max(0, Math.min(scrollY, metrics.maxScrollY));

        let effectiveFixThreshold = metrics.fixThreshold;
        let effectiveBottomThreshold = metrics.bottomThreshold;

        // Apply hysteresis based on scroll direction and current state
        if (currentState === 'normal' && scrollDirection === 'down') {
            effectiveFixThreshold += CONFIG.HYSTERESIS_DOWN;
        } else if (currentState === 'fixed' && scrollDirection === 'up') {
            effectiveFixThreshold -= CONFIG.HYSTERESIS_UP;
        } else if (currentState === 'fixed' && scrollDirection === 'down') {
            effectiveBottomThreshold += CONFIG.HYSTERESIS_DOWN;
        } else if (currentState === 'bottom-aligned' && scrollDirection === 'up') {
            effectiveBottomThreshold -= CONFIG.HYSTERESIS_UP;
        }

        // Determine target state
        if (clampedScrollY < effectiveFixThreshold) {
            return 'normal';
        } else if (clampedScrollY >= effectiveFixThreshold && clampedScrollY < effectiveBottomThreshold) {
            return 'fixed';
        } else {
            return 'bottom-aligned';
        }
    }

    /** Main scroll event handler. */
    function handleScroll() {
        if (isMobile || !metrics) return; // Exit if mobile or metrics not ready

        const scrollY = window.scrollY;

        // --- Overscroll / Rubber Banding ---
        // Check if scrollY is beyond the document limits
        const isCurrentlyOverscrolling = scrollY < 0 || scrollY > metrics.maxScrollY;
        if (isCurrentlyOverscrolling) {
            // If entering overscroll, do nothing yet, let browser handle visually.
            // If already overscrolling, potentially update position if needed (e.g., bottom-aligned)
            // but generally avoid state changes during overscroll.
            // For simplicity, we'll just prevent state changes here.
            lastScrollY = scrollY; // Update lastScrollY to prevent direction miscalculation on exit
            return;
        }

        // --- Determine Scroll Direction ---
        if (scrollY !== lastScrollY) {
            scrollDirection = scrollY > lastScrollY ? 'down' : 'up';
        } else {
            scrollDirection = 'none'; // No change
        }

        // --- Determine Target State ---
        const targetState = getTargetState(scrollY);

        // --- Handle State Transitions ---
        if (targetState !== currentState && !isTransitioning) {
            const now = Date.now();
            if (now - lastStateChangeTime > CONFIG.MIN_CHANGE_INTERVAL) {
                isTransitioning = true;
                // console.log(`Transitioning: ${currentState} -> ${targetState} at scrollY ${Math.round(scrollY)}`);

                // Apply the target state's styles
                switch (targetState) {
                    case 'normal': applyNormalState(); break;
                    case 'fixed': applyFixedState(); break;
                    case 'bottom-aligned': applyBottomAlignedState(scrollY); break;
                }

                currentState = targetState;
                lastStateChangeTime = now;

                // Use setTimeout to end the transition phase after a short duration
                setTimeout(() => {
                    isTransitioning = false;
                    // Re-evaluate state in case scroll position changed significantly during transition
                    // No need to explicitly call handleScroll here, the next scroll event will trigger it.
                    // requestAnimationFrame(handleScroll);
                }, CONFIG.TRANSITION_DURATION);
            }
        }
        // --- Continuous Update Removed ---
        // Styles for 'bottom-aligned' are now set only during the transition *into* the state.
        // No continuous updates needed here as the 'top' value is constant.

        lastScrollY = scrollY; // Update scroll position for next event
    }

    /** Throttled scroll handler using requestAnimationFrame. */
    function onScroll() {
        requestAnimationFrame(handleScroll);
    }

    /** Debounced function generator. */
    function debounce(func, wait) {
        let timeout;
        return function executedFunction(...args) {
            const later = () => {
                clearTimeout(timeout);
                func.apply(this, args);
            };
            clearTimeout(timeout);
            timeout = setTimeout(later, wait);
        };
    }

    /** Handles window resize events. */
    const handleResize = debounce(() => {
        const wasMobile = isMobile;
        isMobile = isMobileView();

        if (isMobile) {
            if (!wasMobile) { // Transitioning to mobile
                resetPositioningStyles();
                currentState = 'normal';
                metrics = null; // Invalidate metrics
                console.log("Switched to mobile view, scroll handler disabled.");
            }
        } else {
            if (wasMobile) { // Transitioning to desktop
                console.log("Switched to desktop view, enabling scroll handler.");
                metrics = calculateMetrics(); // Recalculate immediately
                handleScroll(); // Apply initial state
            } else { // Resizing within desktop view
                metrics = calculateMetrics();
                // Re-apply current state with new metrics
                switch (currentState) {
                    case 'normal': applyNormalState(); break;
                    case 'fixed': applyFixedState(); break;
                    case 'bottom-aligned': applyBottomAlignedState(); break; // Use parameterless version
                }
            }
        }
    }, 250); // Debounce resize events

    /** Checks if error log height has changed significantly. */
    function checkErrorLogHeightChange() {
        if (!errorLogEl || isMobile) return false;
        const currentHeight = errorLogEl.offsetHeight;
        if (Math.abs(currentHeight - lastErrorLogHeight) > CONFIG.ERROR_HEIGHT_TOLERANCE) {
            lastErrorLogHeight = currentHeight;
            return true;
        }
        return false;
    }

    /** Handles content changes that might affect layout. */
    const handleContentChange = debounce(() => {
        if (isMobile) return; // Don't recalculate if mobile

        // console.log("Content change detected, recalculating metrics...");
        const oldState = currentState;
        metrics = calculateMetrics(); // Recalculate metrics

        // Re-apply the state that *should* be active based on current scroll and new metrics
        const targetState = getTargetState(window.scrollY);
        if (targetState !== oldState) {
            // console.log(`State change needed after content update: ${oldState} -> ${targetState}`);
            currentState = targetState; // Update state immediately before applying styles
            lastStateChangeTime = Date.now(); // Reset timer to allow immediate transition
        }

        switch (currentState) {
            case 'normal': applyNormalState(); break;
            case 'fixed': applyFixedState(); break;
            case 'bottom-aligned': applyBottomAlignedState(); break; // Use parameterless version
        }
        // Ensure scroll logic runs again to confirm state after recalculation
        requestAnimationFrame(handleScroll);
    }, 300); // Debounce content changes slightly longer

    /** MutationObserver callback. */
    function handleMutation(mutationsList) {
        if (ignoreMutation || isMobile) {
            // If we just changed styles, ignore the resulting mutations.
            // Also ignore if in mobile view.
            return;
        }
        // Check if any mutation likely affected layout significantly
        for (let mutation of mutationsList) {
            if (mutation.type === 'childList' || mutation.type === 'attributes' || mutation.type === 'characterData') {
                // Check specifically if error log content changed height
                if (errorLogEl && (mutation.target === errorLogEl || errorLogEl.contains(mutation.target))) {
                    if (checkErrorLogHeightChange()) {
                        // console.log("Error log height changed significantly.");
                        handleContentChange();
                        return; // Prioritize error log change
                    }
                }
                // For other changes, trigger a general content change check
                handleContentChange();
                return; // Only need to trigger once per batch
            }
        }
    }

    // --- Initialization ---

    // Initial mobile check
    isMobile = isMobileView();

    if (!isMobile) {
        // Calculate initial metrics only if starting in desktop view
        metrics = calculateMetrics();
        lastScrollY = window.scrollY;
        handleScroll(); // Apply initial state based on load scroll position
        console.log("Scroll handler initialized for desktop view.");

        // Start monitoring error log height changes periodically as a fallback
        if (errorLogEl) {
            setInterval(() => {
                if (checkErrorLogHeightChange()) {
                    // console.log("Error log height changed (polled).");
                    handleContentChange();
                }
            }, CONFIG.ERROR_CHECK_INTERVAL);
        }
    } else {
        console.log("Scroll handler initialized for mobile view (disabled).");
    }

    // --- Event Listeners ---
    window.addEventListener('scroll', onScroll, { passive: true });
    window.addEventListener('resize', handleResize);

    // Set up MutationObserver
    const observer = new MutationObserver(handleMutation);
    const observerConfig = {
        childList: true, // Detect adding/removing child nodes
        subtree: true,   // Observe descendants
        attributes: true, // Detect attribute changes (like style, class)
        characterData: true // Detect text content changes (important for errorLog)
    };
    // Observe both columns for layout changes
    observer.observe(editorColumn, observerConfig);
    observer.observe(toolsColumn, observerConfig);

    // Also listen for CodeMirror changes directly as they might not trigger mutation observer reliably
    if (codeMirrorEl.CodeMirror) {
        codeMirrorEl.CodeMirror.on('change', handleContentChange);
    }

});