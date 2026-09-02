#!/usr/bin/env node
// Byte-compares the WASM build of the firmware renderer against the C++
// goldens. If these ever differ, the browser is not showing what the device
// draws -- which is the entire reason this build exists.
import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const CORPUS = join(HERE, '..', 'corpus');
const GOLDEN = join(HERE, '..', 'golden');
const W = 960, H = 540;
const SEEDS = [
    { name: 'c0_123456', counter: 0, hour: 12, minute: 34, second: 56 },
    { name: 'c7_000000', counter: 7, hour: 0, minute: 0, second: 0 },
    { name: 'c42_235959', counter: 42, hour: 23, minute: 59, second: 59 },
];

function readPGM(p) {
    const buf = readFileSync(p);
    let i = 0, fields = 0;
    while (fields < 4) {
        while (/\s/.test(String.fromCharCode(buf[i]))) i++;
        while (i < buf.length && !/\s/.test(String.fromCharCode(buf[i]))) i++;
        fields++;
    }
    return buf.subarray(i + 1);
}

const factory = (await import(join(HERE, 'out', 'mp_render.js'))).default;
const Module = await factory();

const render = Module.cwrap('mp_render', 'number',
    ['string', 'number', 'number', 'number', 'number', 'number', 'number']);
const pixelsPtr = Module.cwrap('mp_pixels', 'number', []);
const errPtr = Module.cwrap('mp_error', 'number', []);
const dlItems = Module.cwrap('mp_display_list_items', 'number', []);
const msRaster = Module.cwrap('mp_ms_rasterize', 'number', []);

let differing = 0, total = 0;
console.log('WASM firmware renderer vs C++ goldens\n');
for (const f of readdirSync(CORPUS).filter((x) => x.endsWith('.mp'))) {
    const src = readFileSync(join(CORPUS, f), 'utf8');
    for (const seed of SEEDS) {
        const name = `${basename(f, '.mp')}__${seed.name}`;
        total++;
        const ok = render(src, W, H, seed.counter, seed.hour, seed.minute, seed.second);
        if (!ok) {
            console.log(`  ERROR ${name.padEnd(34)} ${Module.UTF8ToString(errPtr())}`);
            differing++; continue;
        }
        const ptr = pixelsPtr();
        const mine = Module.HEAPU8.subarray(ptr, ptr + W * H);
        const golden = readPGM(join(GOLDEN, `${name}.pgm`));
        let d = 0, first = null;
        for (let i = 0; i < W * H; i++) {
            if (mine[i] !== golden[i]) { d++; if (!first) first = [i % W, Math.floor(i / W)]; }
        }
        if (d === 0) {
            console.log(`  SAME  ${name.padEnd(34)} items=${String(dlItems()).padStart(6)} `
                      + `raster=${msRaster().toFixed(1)}ms`);
        } else {
            console.log(`  DIFF  ${name.padEnd(34)} ${d} px, first (${first})`);
            differing++;
        }
    }
}
console.log(`\n${total - differing}/${total} identical`);
process.exit(differing ? 1 : 0);
