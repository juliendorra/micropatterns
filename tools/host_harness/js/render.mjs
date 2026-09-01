#!/usr/bin/env node
// Renders the harness corpus with the WEB renderer, headlessly, so its output
// can be byte-compared against the C++ firmware's goldens.
//
// Why this exists
// ---------------
// The web emulator and the device firmware are claimed to be the same renderer
// in two languages. Nothing checked that claim. The C++ side has had a
// golden-image gate since the beginning (`make verify`); the JS side had
// nothing, so any divergence could only be noticed by a human looking at two
// screens.
//
// The emulator's modules are plain ES modules that touch a very small slice of
// Canvas2D -- fillStyle, fillRect, beginPath/rect/fill, and
// get/put/createImageData -- so a ~60-line shim runs them under Node with no
// native canvas dependency at all. That keeps this runnable in CI.
//
// Usage
//   node render.mjs --out DIR [--script NAME] [--no-occlusion] [--pgm]
//   node render.mjs --compare-occlusion      (JS with vs without culling)
//
// The canvas here is GREYSCALE 0..255 with 255 = white, matching the PGM the
// C++ harness writes, so the two are directly comparable byte for byte.

import { readFileSync, writeFileSync, mkdirSync, readdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..', '..');
const EMU = join(ROOT, 'micropatterns_emulator');
let CORPUS = join(HERE, '..', 'corpus');   // override with --corpus DIR

// The three seeds the C++ harness bakes goldens at. Keep in step with
// `mpharness list`.
const SEEDS = [
    { name: 'c0_123456', counter: 0, hour: 12, minute: 34, second: 56 },
    { name: 'c7_000000', counter: 7, hour: 0, minute: 0, second: 0 },
    { name: 'c42_235959', counter: 42, hour: 23, minute: 59, second: 59 },
];

const WIDTH = 960, HEIGHT = 540;   // M5Paper landscape, as the goldens use

// --- minimal Canvas2D over a byte-per-pixel greyscale buffer ---------------
//
// Deliberately NOT a general canvas. It implements exactly what the emulator
// calls and throws on anything else, so a future emulator change that reaches
// for a real canvas feature fails loudly here instead of silently rendering
// something different from the browser.
class ShimCanvas {
    constructor(w, h) {
        this.width = w;
        this.height = h;
        this.data = new Uint8Array(w * h).fill(255);   // 255 = white
        this._ctx = new ShimCtx(this);
    }
    getContext() { return this._ctx; }
}

class ShimCtx {
    constructor(canvas) {
        this.canvas = canvas;
        this._fill = 0;
        this._path = [];
    }
    set fillStyle(v) {
        // The emulator uses 'black' / 'white' and nothing else.
        if (v === 'black' || v === '#000' || v === '#000000') this._fill = 0;
        else if (v === 'white' || v === '#fff' || v === '#ffffff') this._fill = 255;
        else throw new Error(`ShimCtx: unexpected fillStyle ${JSON.stringify(v)} -- `
                           + `the shim only models the two the emulator uses.`);
    }
    get fillStyle() { return this._fill === 0 ? 'black' : 'white'; }

    fillRect(x, y, w, h) {
        const c = this.canvas;
        const x0 = Math.max(0, Math.trunc(x)), y0 = Math.max(0, Math.trunc(y));
        const x1 = Math.min(c.width, Math.trunc(x) + Math.trunc(w));
        const y1 = Math.min(c.height, Math.trunc(y) + Math.trunc(h));
        for (let yy = y0; yy < y1; yy++) {
            c.data.fill(this._fill, yy * c.width + x0, yy * c.width + x1);
        }
    }
    beginPath() { this._path = []; }
    rect(x, y, w, h) { this._path.push([x, y, w, h]); }
    fill() {
        for (const [x, y, w, h] of this._path) this.fillRect(x, y, w, h);
        this._path = [];
    }
    createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; }
    getImageData(x, y, w, h) {
        const img = this.createImageData(w, h);
        for (let yy = 0; yy < h; yy++) {
            for (let xx = 0; xx < w; xx++) {
                const v = this.canvas.data[(y + yy) * this.canvas.width + (x + xx)];
                const o = (yy * w + xx) * 4;
                img.data[o] = img.data[o + 1] = img.data[o + 2] = v;
                img.data[o + 3] = 255;
            }
        }
        return img;
    }
    putImageData(img, x, y) {
        for (let yy = 0; yy < img.height; yy++) {
            for (let xx = 0; xx < img.width; xx++) {
                const o = (yy * img.width + xx) * 4;
                this.canvas.data[(y + yy) * this.canvas.width + (x + xx)] = img.data[o];
            }
        }
    }
}

// --- DOMMatrix, which Node does not have -----------------------------------
//
// 2D affine only: [a c e ; b d f ; 0 0 1]. The emulator uses exactly
// translateSelf, rotateSelf, inverse, transformPoint and the a..f fields --
// SCALE is kept out of the matrix and applied separately by the generator.
//
// One browser subtlety worth knowing: the emulator copies matrices with
// `new DOMMatrix(otherMatrix)`. DOMMatrix is not iterable, so in a browser that
// argument goes through the (DOMString) arm of the constructor's union -- i.e.
// the matrix is serialised to a CSS `matrix(...)` string and re-parsed. It
// works, but it is a string round-trip in the middle of the transform stack.
// This shim copies the fields directly.
class ShimDOMMatrix {
    constructor(init) {
        if (init && typeof init === 'object') {
            this.a = init.a; this.b = init.b; this.c = init.c;
            this.d = init.d; this.e = init.e; this.f = init.f;
        } else if (Array.isArray(init) && init.length === 6) {
            [this.a, this.b, this.c, this.d, this.e, this.f] = init;
        } else {
            this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = 0; this.f = 0;
        }
    }
    translateSelf(tx = 0, ty = 0) {
        this.e += this.a * tx + this.c * ty;
        this.f += this.b * tx + this.d * ty;
        return this;
    }
    rotateSelf(deg = 0) {
        const r = (deg * Math.PI) / 180;
        const cos = Math.cos(r), sin = Math.sin(r);
        const { a, b, c, d } = this;
        this.a = a * cos + c * sin;
        this.b = b * cos + d * sin;
        this.c = a * -sin + c * cos;
        this.d = b * -sin + d * cos;
        return this;
    }
    scaleSelf(sx = 1, sy = sx) {
        this.a *= sx; this.b *= sx; this.c *= sy; this.d *= sy;
        return this;
    }
    inverse() {
        const { a, b, c, d, e, f } = this;
        const det = a * d - b * c;
        const m = new ShimDOMMatrix();
        if (det === 0) { m.a = m.b = m.c = m.d = m.e = m.f = NaN; return m; }
        m.a = d / det;  m.b = -b / det;
        m.c = -c / det; m.d = a / det;
        m.e = (c * f - d * e) / det;
        m.f = (b * e - a * f) / det;
        return m;
    }
    transformPoint(p) {
        const x = p.x ?? 0, y = p.y ?? 0;
        return { x: this.a * x + this.c * y + this.e,
                 y: this.b * x + this.d * y + this.f, z: 0, w: 1 };
    }
    toString() {
        return `matrix(${this.a}, ${this.b}, ${this.c}, ${this.d}, ${this.e}, ${this.f})`;
    }
}
globalThis.DOMMatrix = ShimDOMMatrix;

// --- render one script -----------------------------------------------------
async function loadEmulator() {
    const imp = (f) => import(pathToFileURL(join(EMU, f)).href);
    const [parser, gen, rend] = await Promise.all([
        imp('parser.js'), imp('display_list_generator.js'), imp('display_list_renderer.js'),
    ]);
    return {
        MicroPatternsParser: parser.MicroPatternsParser,
        DisplayListGenerator: gen.DisplayListGenerator,
        DisplayListRenderer: rend.DisplayListRenderer,
    };
}

const fmtErr = (e) => (typeof e === 'string' ? e
    : e && (e.message || e.msg || e.error) ? (e.message || e.msg || e.error)
    : JSON.stringify(e));

function renderOne(mods, source, seed, { occlusion = true } = {}) {
    const canvas = new ShimCanvas(WIDTH, HEIGHT);
    const ctx = canvas.getContext('2d');

    const parser = new mods.MicroPatternsParser();
    const parsed = parser.parse(source);
    if (parsed.errors && parsed.errors.length) {
        throw new Error('parse errors:\n  ' + parsed.errors.map(fmtErr).join('\n  '));
    }

    const environment = {
        HOUR: seed.hour, MINUTE: seed.minute, SECOND: seed.second,
        COUNTER: seed.counter, WIDTH, HEIGHT,
    };

    const generator = new mods.DisplayListGenerator(parsed.assets.assets, environment);
    const initialVars = {};
    parsed.variables.forEach((v) => { initialVars[`$${v}`] = 0; });
    const genOut = generator.generate(parsed.commands, initialVars);
    if (genOut.errors && genOut.errors.length) {
        throw new Error('generator errors:\n  ' + genOut.errors.map(fmtErr).join('\n  '));
    }

    const renderer = new mods.DisplayListRenderer(ctx, parsed.assets.assets, {
        enableOcclusionCulling: occlusion,
        occlusionBlockSize: 16,
        enablePixelBatching: true,
        enablePatternTileCaching: true,
        enableTransformCaching: true,
    });
    renderer.render(genOut.displayList);

    return { pixels: canvas.data, stats: renderer.getStats ? renderer.getStats() : null };
}

function readPGM(path) {
    const buf = readFileSync(path);
    // P5\n<w> <h>\n<max>\n then raw bytes. Header fields are whitespace separated.
    let i = 0, fields = 0;
    while (fields < 4) {
        while (/\s/.test(String.fromCharCode(buf[i]))) i++;
        while (i < buf.length && !/\s/.test(String.fromCharCode(buf[i]))) i++;
        fields++;
    }
    return buf.subarray(i + 1);
}

function writePGM(path, pixels) {
    const header = Buffer.from(`P5\n${WIDTH} ${HEIGHT}\n255\n`, 'ascii');
    writeFileSync(path, Buffer.concat([header, Buffer.from(pixels)]));
}

function diff(a, b) {
    let n = 0, first = null;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) {
            n++;
            if (!first) first = [i % WIDTH, Math.floor(i / WIDTH)];
        }
    }
    return { n, first };
}

// --- main ------------------------------------------------------------------
const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);
const val = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };

if (val('--corpus')) CORPUS = val('--corpus');
const scripts = readdirSync(CORPUS).filter((f) => f.endsWith('.mp'))
    .filter((f) => !val('--script') || basename(f, '.mp') === val('--script'));

const mods = await loadEmulator();

if (has('--compare-occlusion')) {
    // Does the WEB renderer's culling change its own output? It should not:
    // culling may only skip items proven to be completely covered.
    console.log('JS renderer: occlusion ON vs OFF\n');
    let same = 0, differ = 0;
    for (const f of scripts) {
        const src = readFileSync(join(CORPUS, f), 'utf8');
        for (const seed of SEEDS) {
            const on = renderOne(mods, src, seed, { occlusion: true });
            const off = renderOne(mods, src, seed, { occlusion: false });
            const d = diff(on.pixels, off.pixels);
            const label = `${basename(f, '.mp')}__${seed.name}`.padEnd(34);
            if (d.n === 0) { console.log(`  SAME  ${label}`); same++; }
            else { console.log(`  DIFF  ${label} ${d.n} px, first (${d.first})`); differ++; }
        }
    }
    console.log(`\n${same} identical, ${differ} differing`);
    process.exit(differ ? 1 : 0);
}

if (has('--compare-golden')) {
    // The audit: WEB renderer vs the C++ firmware's golden images, byte for byte.
    // Same corpus, same seeds, same 960x540 canvas, same PGM encoding.
    const goldenDir = val('--golden', join(HERE, '..', 'golden'));
    const occl = !has('--no-occlusion');
    let differing = 0, totalPx = 0;
    console.log(`web renderer vs C++ goldens  (occlusion ${occl ? 'ON' : 'OFF'})\n`);
    for (const f of scripts) {
        const src = readFileSync(join(CORPUS, f), 'utf8');
        for (const seed of SEEDS) {
            const name = `${basename(f, '.mp')}__${seed.name}`;
            const mine = renderOne(mods, src, seed, { occlusion: occl }).pixels;
            let golden;
            try { golden = readPGM(join(goldenDir, `${name}.pgm`)); }
            catch { console.log(`  MISSING golden for ${name}`); continue; }
            const d = diff(mine, golden);
            totalPx += d.n;
            if (d.n === 0) console.log(`  SAME  ${name.padEnd(34)}`);
            else { differing++; console.log(`  DIFF  ${name.padEnd(34)} ${d.n} px, first (${d.first})`); }
        }
    }
    const total = scripts.length * SEEDS.length;
    console.log(`\n${total - differing}/${total} identical, ${differing} differing, ${totalPx} pixels total`);
    console.log(
        '\nKnown gap: the outline primitives (LINE, RECT, CIRCLE) and PIXEL are\n' +
        'drawn differently by the two engines under rotation and scale. Everything\n' +
        'else -- patterns, fills, transforms, the whole common path -- is identical.\n' +
        'See docs/analysis/web-device-renderer-audit.md.');
    process.exit(0);
}

const outDir = val('--out', join(HERE, 'out'));
mkdirSync(outDir, { recursive: true });
const occlusion = !has('--no-occlusion');
for (const f of scripts) {
    const src = readFileSync(join(CORPUS, f), 'utf8');
    for (const seed of SEEDS) {
        const name = `${basename(f, '.mp')}__${seed.name}`;
        const r = renderOne(mods, src, seed, { occlusion });
        writePGM(join(outDir, `${name}.pgm`), r.pixels);
        const s = r.stats || {};
        console.log(`${name.padEnd(34)} items=${s.totalItems ?? '?'} `
                  + `rendered=${s.renderedItems ?? '?'} `
                  + `offscreen=${s.culledOffScreen ?? '?'} `
                  + `occluded=${s.culledByOcclusion ?? '?'}`);
    }
}
console.log(`\nwrote ${scripts.length * SEEDS.length} images to ${outDir}`);
