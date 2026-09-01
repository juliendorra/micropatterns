// The DEVICE renderer, in the browser.
//
// This is not a third implementation of MicroPatterns -- it is the firmware's
// own C++ renderer and parser, compiled to WebAssembly from the same six source
// files the M5Paper and the Watchy run. Byte-identical to the device: see
// `make verify-wasm` in tools/host_harness, which checks it against the same
// golden images the firmware is gated on.
//
// Rebuild the .wasm with `tools/host_harness/wasm/build.sh` and copy the two
// files in wasm/ across. They are committed because this page is deployed as
// static files over FTP with no build step.

let modulePromise = null;

// Loaded on demand: most sessions never switch to this path, and the module is
// ~100KB. Failure is reported to the caller rather than thrown at load time, so
// a browser without WebAssembly degrades to "this path is unavailable" instead
// of breaking the editor.
function loadModule() {
    if (!modulePromise) {
        modulePromise = import('./wasm/mp_render.mjs')
            .then((m) => m.default())
            .catch((e) => { modulePromise = null; throw e; });
    }
    return modulePromise;
}

export class DeviceRenderer {
    constructor() {
        this.mod = null;
    }

    async ready() {
        if (!this.mod) {
            const M = await loadModule();
            this.mod = {
                M,
                render: M.cwrap('mp_render', 'number',
                    ['string', 'number', 'number', 'number', 'number', 'number', 'number']),
                pixels: M.cwrap('mp_pixels', 'number', []),
                error: M.cwrap('mp_error', 'number', []),
                parse: M.cwrap('mp_parse', 'number', ['string']),
                errCount: M.cwrap('mp_parse_error_count', 'number', []),
                errAt: M.cwrap('mp_parse_error_at', 'number', ['number']),
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
        }
        return this.mod;
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

    // Renders into a 2D context. Returns { ok, error, stats }.
    async render(source, ctx, env) {
        const m = await this.ready();
        const w = ctx.canvas.width, h = ctx.canvas.height;

        const ok = m.render(source, w, h,
            env.COUNTER | 0, env.HOUR | 0, env.MINUTE | 0, env.SECOND | 0);
        if (!ok) {
            return { ok: false, error: m.M.UTF8ToString(m.error()), stats: null };
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
            stats: {
                totalItems: s.items(), renderedItems: s.rendered(),
                culledOffScreen: s.offscreen(), culledByOcclusion: s.occluded(),
                msParse: s.msParse(), msDisplayList: s.msDisplayList(),
                msRasterize: s.msRasterize(),
            },
        };
    }
}
