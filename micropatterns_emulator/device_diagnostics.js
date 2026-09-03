// Stable boundary between a rendering engine and editor UX.
//
// Engines may expose exceptions, numeric telemetry, JSON, or something else in
// the future. DeviceRenderer adapts that raw output here. The rest of the editor
// only consumes this versioned shape and therefore does not need to be rewired
// when the engine implementation changes.
export const DEVICE_DIAGNOSTIC_SCHEMA_VERSION = 1;

// Phase 2 covers the whole front half: compiling (parse + program build) at
// sync time, or loading the stored program at render time -- the device
// reports both under the same allocator phase. The `stage` on the result says
// which one was running.
const PHASES = ['idle', 'source loading', 'compiling or loading the program', 'display-list generation',
    'rasterization', 'browser output'];
const SOURCES = ['an explicit allocation', 'C++/STL', 'Arduino String',
    'a radio reservation'];
const CAPABILITIES = ['the default 8-bit heap', 'internal RAM', 'PSRAM'];
const STATES = ['radios off', 'BLE active', 'Wi-Fi/TLS active'];

const bytes = (value) => `${Number(value || 0).toLocaleString('en-US')} B`;

function sourceTextAt(source, line) {
    if (!line || !source) return null;
    return source.split(/\r?\n/)[line - 1]?.trim() || null;
}

function diagnostic(fields, source) {
    return {
        schemaVersion: DEVICE_DIAGNOSTIC_SCHEMA_VERSION,
        severity: 'error',
        code: 'MP_RUNTIME_ERROR',
        origin: 'runtime',
        line: null,
        title: 'Render failed',
        message: '',
        explanation: '',
        suggestions: [],
        evidence: [],
        ...fields,
        sourceText: fields.sourceText ?? sourceTextAt(source, fields.line),
    };
}

export function diagnosticFromParseError(rawError, source = '') {
    return diagnostic({
        severity: 'error',
        code: 'MP_PARSE_ERROR',
        origin: 'parser',
        line: rawError.line,
        title: rawError.line ? `Parser stopped at line ${rawError.line}` : 'Parser rejected the script',
        message: rawError.message,
        explanation: 'The device firmware parser would reject this script before rendering.',
        suggestions: ['Fix this error first; device resource checks only run after a valid parse.'],
        evidence: rawError.raw ? [{ label: 'Firmware error', value: rawError.raw }] : [],
    }, source);
}

function lineFromRuntimeError(error) {
    const match = String(error || '').match(/(?:Line\s+|line=)(\d+)/i);
    return match ? Number(match[1]) : null;
}

function oomDiagnostic(error, memory, source, stage) {
    const failure = memory.failure;
    const profile = memory.profile === 'watchy2' ? 'Watchy 2' : memory.profile;
    const phase = PHASES[failure.phase] || `phase ${failure.phase}`;
    const allocator = SOURCES[failure.source] || `allocation source ${failure.source}`;
    const capability = CAPABILITIES[failure.capability] || `capability ${failure.capability}`;
    const fragmentedInternal = failure.request <= failure.internalFree &&
        failure.request > failure.internalLargest;
    const fragmentedPsram = failure.request <= failure.psramFree &&
        failure.request > failure.psramLargest;
    const fragmented = fragmentedInternal || fragmentedPsram;

    const compiling = stage === 'compile';
    const suggestions = phase === 'display-list generation' ? [
        'Reduce how many drawing commands this line and its enclosing REPEAT blocks expand into.',
        'For tiled scenes, increase tile size or reduce the number of layers and strokes.',
        'A compact display list or streaming renderer addresses this allocation; VM bytecode alone does not.',
    ] : phase === PHASES[2] && compiling ? [
        'Reduce pattern data, declarations, or deeply nested blocks near this line.',
        'The device compiles during sync with radios off; this is not a wake-time reboot.',
    ] : phase === PHASES[2] ? [
        'Reduce the stored program or asset data loaded near this line.',
        'Re-sync after a firmware update so the cached program format is current.',
    ] : [
        'Reduce the amount of work or temporary memory required near the highlighted line.',
        'Try the radios-off state to distinguish script pressure from radio memory pressure.',
    ];

    return diagnostic({
        severity: 'error',
        code: 'MP_DEVICE_OOM',
        origin: 'device-model',
        line: failure.line || lineFromRuntimeError(error),
        title: compiling
            ? `${profile} would reject this script during sync: out of memory`
            : `${profile} would reboot: out of memory`,
        message: `${bytes(failure.request)} requested from ${capability} during ${phase}.`,
        explanation: fragmented
            ? `Enough bytes remain in total, but no contiguous block is large enough. This is allocator fragmentation, not just aggregate memory use.`
            : `The simulated ${profile} heap cannot satisfy this allocation with ${STATES[memory.deviceState] || 'the selected device state'}.`,
        suggestions,
        evidence: [
            { label: 'Allocation path', value: allocator },
            { label: 'Internal free', value: bytes(failure.internalFree) },
            { label: 'Largest internal block', value: bytes(failure.internalLargest) },
            ...(failure.psramFree || failure.psramLargest ? [
                { label: 'PSRAM free', value: bytes(failure.psramFree) },
                { label: 'Largest PSRAM block', value: bytes(failure.psramLargest) },
            ] : []),
            { label: 'Budget confidence', value: memory.stateCalibrated ? 'calibrated' : 'provisional' },
        ],
        profile: memory.profile,
        deviceState: memory.deviceState,
        stage,
        raw: error,
    }, source);
}

export function diagnosticsForRender({ ok, error, memory, stats, source = '', stage = 'render' }) {
    if (!ok) {
        if (memory?.failure) return [oomDiagnostic(error, memory, source, stage)];
        const line = lineFromRuntimeError(error);
        return [diagnostic({
            code: stage === 'compile' ? 'MP_COMPILE_ERROR' : 'MP_RUNTIME_ERROR',
            line,
            title: stage === 'compile' ? 'Script compilation failed' : 'Render failed',
            message: error || 'The renderer stopped without a structured error.',
            explanation: stage === 'compile'
                ? 'The firmware could not compile this script into the program stored during sync.'
                : 'The firmware renderer did not complete this frame.',
            suggestions: ['Inspect the highlighted command and the firmware error text.'],
            evidence: error ? [{ label: 'Runtime error', value: error }] : [],
            stage,
            raw: error,
        }, source)];
    }

    if (!memory?.constrained || !stats) return [];
    const out = [];

    if (!stats.occupancyMapUsed && stats.totalItems > 0) {
        out.push(diagnostic({
            severity: 'warning',
            code: 'MP_OCCUPANCY_FALLBACK',
            origin: 'device-model',
            title: 'Low-memory painter-order fallback is active',
            message: 'The frame is correct, but the renderer could not use its occupancy map.',
            explanation: 'More overlapped pixels must be redrawn. Dense scenes can therefore take longer on the device and have less watchdog margin.',
            suggestions: [
                'Reduce overlapping layers or tile density if device rendering is slow.',
                'Test a sweep of counters and seconds; this fallback can appear only at high-memory frames.',
            ],
            evidence: [
                { label: 'Display-list items', value: String(stats.totalItems) },
                { label: 'Rendered items', value: String(stats.renderedItems) },
                { label: 'Off-screen items', value: String(stats.culledOffScreen) },
            ],
            profile: memory.profile,
            deviceState: memory.deviceState,
        }, source));
    }

    const internalRatio = memory.internal.initialFree
        ? memory.internal.peakUsed / memory.internal.initialFree : 0;
    const psramRatio = memory.psram.initialFree
        ? memory.psram.peakUsed / memory.psram.initialFree : 0;
    const peakRatio = Math.max(internalRatio, psramRatio);
    if (peakRatio >= 0.85) {
        const internalIsPeak = internalRatio >= psramRatio;
        const line = internalIsPeak ? memory.internal.peakLine : memory.psram.peakLine;
        out.push(diagnostic({
            severity: 'warning',
            code: 'MP_MEMORY_PRESSURE',
            origin: 'device-model',
            line: line || null,
            title: 'This render is close to the device memory limit',
            message: `Peak simulated heap use reached ${Math.round(peakRatio * 100)}% of the selected budget.`,
            explanation: 'Small script, radio-state, or allocator-layout changes may turn this successful frame into a reboot.',
            suggestions: [
                'Reduce repeated drawing commands or pattern/state churn near the highlighted line.',
                'Run the script across device states and time/counter inputs before publishing it.',
            ],
            evidence: [
                { label: 'Peak internal use', value: bytes(memory.internal.peakUsed) },
                { label: 'Peak PSRAM use', value: bytes(memory.psram.peakUsed) },
                { label: 'Budget confidence', value: memory.stateCalibrated ? 'calibrated' : 'provisional' },
            ],
            profile: memory.profile,
            deviceState: memory.deviceState,
        }, source));
    }

    if (stats.displayListBytes >= 65536 && peakRatio < 0.85) {
        out.push(diagnostic({
            severity: 'warning',
            code: 'MP_LARGE_DISPLAY_LIST',
            origin: 'runtime',
            line: memory.internal.peakLine || memory.psram.peakLine || null,
            title: 'Large display list',
            message: `${stats.totalItems.toLocaleString()} items occupy about ${bytes(stats.displayListBytes)} before rasterization.`,
            explanation: 'The script renders successfully now, but list growth needs increasingly large contiguous allocations.',
            suggestions: ['Reduce expanded REPEAT output or tile density, especially on Watchy.'],
            evidence: [{ label: 'Display-list bytes', value: bytes(stats.displayListBytes) }],
            profile: memory.profile,
            deviceState: memory.deviceState,
        }, source));
    }

    return out;
}
