#!/usr/bin/env node
// Keep every consumer pointed at the one shared renderer implementation.
// Compilation catches API breakage; this catches a build silently omitting or
// replacing one of the shared translation units.
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const CORE = [
    'micropatterns_parser.cpp',
    'micropatterns_runtime.cpp',
    'micropatterns_drawing.cpp',
    'display_list_renderer.cpp',
    'occlusion_buffer.cpp',
    'matrix_utils.cpp',
];

const consumers = [
    ['native host harness', 'tools/host_harness/Makefile'],
    ['canonical WASM', 'tools/host_harness/wasm/build.sh'],
    ['constrained WASM', 'tools/host_harness/wasm/build_constrained.sh'],
    ['Watchy firmware', 'Watchy_MicroPatterns/platformio.ini'],
    ['M5Paper firmware', 'M5Paper_MicroPatterns/platformio.ini'],
];

for (const [name, path] of consumers) {
    const text = readFileSync(join(ROOT, path), 'utf8');
    for (const source of CORE) {
        assert.ok(text.includes(source), `${name} does not include shared ${source}`);
    }
}

for (const source of CORE) {
    assert.equal(existsSync(join(ROOT, 'Watchy_MicroPatterns', 'src', source)), false,
        `Watchy has a private ${source}; it must compile the M5Paper shared source`);
}

console.log(`PASS  ${CORE.length} shared renderer sources across ${consumers.length} consumers`);
