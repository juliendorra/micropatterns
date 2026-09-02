// The DEVICE renderer, in the browser.
//
// This is not a third implementation of MicroPatterns -- it is the firmware's
// own C++ renderer and parser, compiled to WebAssembly from the same six source
// files the M5Paper and the Watchy run. Successful pixel output is checked
// byte-for-byte against the canonical firmware WASM build; device resource
// fidelity and its documented calibration limits are a separate layer.
//
// Rebuild with `tools/host_harness/wasm/build.sh` and copy the two files in
// wasm/ across. The glue is mp_render.js -- see build.sh for why not .mjs. They are committed because this page is deployed as
// static files over FTP with no build step.

// .js, never .mjs: the static host serves .mjs with no Content-Type and the
// browser refuses to execute it as a module. See tools/host_harness/wasm/build.sh.
//
// The two per-device constrained builds come from build_constrained.sh. If a
// static deployment omits either artifact, asking for it falls back to the
// canonical reference module instead of breaking the editor.
const MODULE_PATHS = {
    reference: './wasm/mp_render.js',
    watchy: './wasm/mp_render_watchy.js',
    m5paper: './wasm/mp_render_m5paper.js',
};
const modulePromises = new Map();

// Loaded on demand: most sessions never switch to this path, and the module is
// ~100KB. Failure is reported to the caller rather than thrown at load time, so
// a browser without WebAssembly degrades to "this path is unavailable" instead
// of breaking the editor.
function loadModule(profile) {
    if (!MODULE_PATHS[profile]) throw new Error(`Unknown device profile: ${profile}`);
    if (!modulePromises.has(profile)) {
        const promise = import(MODULE_PATHS[profile])
            .then((m) => m.default())
            .catch((e) => {
                modulePromises.delete(profile);
                if (profile !== 'reference') {
                    // A missing or broken variant must not take the editor down.
                    console.warn(`Device profile '${profile}' unavailable (${e && e.message ? e.message : e}); using 'reference'.`);
                    return loadModule('reference');
                }
                throw e;
            });
        modulePromises.set(profile, promise);
    }
    return modulePromises.get(profile);
}

export class DeviceRenderer {
    // Default is the committed, deployed build. Callers opt into a device
    // profile explicitly once its module actually ships.
    constructor(profile = 'reference') {
        this.profile = profile;
        this.deviceState = 0;
        this.modules = new Map();
    }

    setProfile(profile) {
        if (!MODULE_PATHS[profile]) throw new Error(`Unknown device profile: ${profile}`);
        this.profile = profile;
    }

    setDeviceState(state) {
        const value = Number(state);
        this.deviceState = value >= 0 && value <= 2 ? value : 0;
    }

    async ready() {
        const profile = this.profile;
        if (!this.modules.has(profile)) {
            const M = await loadModule(profile);
            // Decide by what the module actually exports, not by the name asked
            // for: a fallback to `reference` must not try to cwrap symbols that
            // are not there.
            const constrained = typeof M._mp_set_device_state === 'function';
            const wrapped = {
                M,
                constrained,
                render: M.cwrap('mp_render', 'number',
                    ['string', 'number', 'number', 'number', 'number', 'number', 'number']),
                pixels: M.cwrap('mp_pixels', 'number', []),
                error: M.cwrap('mp_error', 'number', []),
                parse: M.cwrap('mp_parse', 'number', ['string']),
                errCount: M.cwrap('mp_parse_error_count', 'number', []),
                errAt: M.cwrap('mp_parse_error_at', 'number', ['number']),
                assetCount: M.cwrap('mp_asset_count', 'number', []),
                assetName: M.cwrap('mp_asset_name', 'number', ['number']),
                assetOriginalName: M.cwrap('mp_asset_original_name', 'number', ['number']),
                assetWidth: M.cwrap('mp_asset_width', 'number', ['number']),
                assetHeight: M.cwrap('mp_asset_height', 'number', ['number']),
                assetData: M.cwrap('mp_asset_data', 'number', ['number']),
                stats: {
                    items: M.cwrap('mp_display_list_items', 'number', []),
                    rendered: M.cwrap('mp_rendered_items', 'number', []),
                    offscreen: M.cwrap('mp_culled_offscreen', 'number', []),
                    occluded: M.cwrap('mp_culled_occlusion', 'number', []),
                    msParse: M.cwrap('mp_ms_parse', 'number', []),
                    msDisplayList: M.cwrap('mp_ms_displaylist', 'number', []),
                    msRasterize: M.cwrap('mp_ms_rasterize', 'number', []),
                },
            };
            if (constrained) {
                const number = (name) => M.cwrap(name, 'number', []);
                wrapped.setDeviceState = M.cwrap('mp_set_device_state', null, ['number']);
                wrapped.profileName = M.cwrap('mp_device_profile', 'number', []);
                wrapped.arduinoVersion = M.cwrap('mp_device_arduino', 'number', []);
                wrapped.idfVersion = M.cwrap('mp_device_idf', 'number', []);
                wrapped.memory = {
                    calibrated: number('mp_device_profile_calibrated'),
                    stateCalibrated: number('mp_device_state_calibrated'),
                    allocationCalls: number('mp_mem_allocation_calls'),
                    reallocCalls: number('mp_mem_realloc_calls'),
                    freeCalls: number('mp_mem_free_calls'),
                    initialInternalFree: number('mp_mem_initial_internal_free'),
                    initialInternalLargest: number('mp_mem_initial_internal_largest'),
                    currentInternalFree: number('mp_mem_current_internal_free'),
                    currentInternalLargest: number('mp_mem_current_internal_largest'),
                    peakInternalUsed: number('mp_mem_peak_internal_used'),
                    initialPsramFree: number('mp_mem_initial_psram_free'),
                    currentPsramFree: number('mp_mem_current_psram_free'),
                    peakPsramUsed: number('mp_mem_peak_psram_used'),
                    failureValid: number('mp_mem_failure_valid'),
                    failureRequest: number('mp_mem_failure_request'),
                    failurePhase: number('mp_mem_failure_phase'),
                    failureSource: number('mp_mem_failure_source'),
                    failureCapability: number('mp_mem_failure_capability'),
                    failureInternalFree: number('mp_mem_failure_internal_free'),
                    failureInternalLargest: number('mp_mem_failure_internal_largest'),
                    failurePsramFree: number('mp_mem_failure_psram_free'),
                    failurePsramLargest: number('mp_mem_failure_psram_largest'),
                };
            }
            this.modules.set(profile, wrapped);
        }
        return this.modules.get(profile);
    }

    memorySnapshot(m) {
        if (!m.constrained) {
            return { profile: 'reference', arduinoVersion: null, idfVersion: null,
                calibrated: false, stateCalibrated: false,
                deviceState: this.deviceState, constrained: false, failure: null };
        }
        const x = m.memory;
        const failed = !!x.failureValid();
        return {
            profile: m.M.UTF8ToString(m.profileName()),
            arduinoVersion: m.M.UTF8ToString(m.arduinoVersion()),
            idfVersion: m.M.UTF8ToString(m.idfVersion()),
            calibrated: !!x.calibrated(),
            stateCalibrated: !!x.stateCalibrated(),
            constrained: true,
            deviceState: this.deviceState,
            allocationCalls: x.allocationCalls(),
            reallocCalls: x.reallocCalls(),
            freeCalls: x.freeCalls(),
            internal: {
                initialFree: x.initialInternalFree(),
                initialLargest: x.initialInternalLargest(),
                currentFree: x.currentInternalFree(),
                currentLargest: x.currentInternalLargest(),
                peakUsed: x.peakInternalUsed(),
            },
            psram: {
                initialFree: x.initialPsramFree(),
                currentFree: x.currentPsramFree(),
                peakUsed: x.peakPsramUsed(),
            },
            failure: failed ? {
                request: x.failureRequest(),
                phase: x.failurePhase(),
                source: x.failureSource(),
                capability: x.failureCapability(),
                internalFree: x.failureInternalFree(),
                internalLargest: x.failureInternalLargest(),
                psramFree: x.failurePsramFree(),
                psramLargest: x.failurePsramLargest(),
            } : null,
        };
    }

    // Runs the FIRMWARE's parser and returns its diagnostics.
    //
    // The editor's own parser is a second implementation and has been caught
    // disagreeing with this one -- it rejected LINE's X1/Y1/X2/Y2 outright
    // because its argument-key regex had no digits. These are the errors the
    // device will actually produce.
    //
    // Returns [{ line, message, raw }]. `line` is null when the firmware did
    // not attribute the error to a line.
    async lint(source) {
        const m = await this.ready();
        m.parse(source);
        const out = [];
        for (let i = 0; i < m.errCount(); i++) {
            const raw = m.M.UTF8ToString(m.errAt(i));
            const match = raw.match(/^Line\s+(\d+):\s*(.*)$/);
            out.push(match
                ? { line: parseInt(match[1], 10), message: match[2], raw }
                : { line: null, message: raw, raw });
        }
        return out;
    }

    // The patterns the last lint() found, keyed by uppercase name, in the shape
    // the editor's preview panel and pixel editor already consume:
    //   { name, originalName, width, height, data: number[] of 0/1 }
    //
    // Plain arrays, not views into the wasm heap: the pixel editor mutates
    // asset.data in place and writes it back into the script text, and a heap
    // view could be invalidated by the next call that grows memory.
    //
    // Call lint() first -- this reads whatever the firmware parser last saw.
    // Assets defined before a parse error are still present, so a broken
    // script keeps its patterns editable.
    async assets() {
        const m = await this.ready();
        const out = {};
        const n = m.assetCount();
        for (let i = 0; i < n; i++) {
            const w = m.assetWidth(i), h = m.assetHeight(i), ptr = m.assetData(i);
            const name = m.M.UTF8ToString(m.assetName(i));
            out[name] = {
                name,
                originalName: m.M.UTF8ToString(m.assetOriginalName(i)),
                width: w, height: h,
                data: Array.from(m.M.HEAPU8.subarray(ptr, ptr + w * h)),
            };
        }
        return out;
    }

    // Renders into a 2D context. Returns { ok, error, stats }.
    async render(source, ctx, env) {
        const m = await this.ready();
        const w = ctx.canvas.width, h = ctx.canvas.height;

        if (m.constrained) m.setDeviceState(this.deviceState);
        const ok = m.render(source, w, h,
            env.COUNTER | 0, env.HOUR | 0, env.MINUTE | 0, env.SECOND | 0);
        const memory = this.memorySnapshot(m);
        if (!ok) {
            return { ok: false, error: m.M.UTF8ToString(m.error()), stats: null, memory };
        }

        // 8-bit greyscale out of the wasm heap, straight into ImageData. The
        // subarray is a view onto the module's memory and can be invalidated by
        // a later call that grows it, so it is consumed immediately.
        const ptr = m.pixels();
        const gray = m.M.HEAPU8.subarray(ptr, ptr + w * h);
        const img = ctx.createImageData(w, h);
        for (let i = 0, o = 0; i < gray.length; i++, o += 4) {
            const v = gray[i];
            img.data[o] = img.data[o + 1] = img.data[o + 2] = v;
            img.data[o + 3] = 255;
        }
        ctx.putImageData(img, 0, 0);

        const s = m.stats;
        return {
            ok: true,
            error: null,
            memory,
            stats: {
                totalItems: s.items(), renderedItems: s.rendered(),
                culledOffScreen: s.offscreen(), culledByOcclusion: s.occluded(),
                msParse: s.msParse(), msDisplayList: s.msDisplayList(),
                msRasterize: s.msRasterize(),
            },
        };
    }
}
