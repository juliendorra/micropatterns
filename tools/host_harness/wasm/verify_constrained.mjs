#!/usr/bin/env node
// End-to-end checks for the versioned device-resource WASM builds. Successful
// renders must remain byte-identical to the unconstrained firmware oracle;
// resource failures are checked separately from pixel behavior.
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..', '..');
const SEED = { counter: 0, hour: 12, minute: 34, second: 56 };

const source = (path) => readFileSync(join(ROOT, path), 'utf8');
const prims = source('tools/host_harness/corpus/prims.mp');
const artDeco = source('tools/host_harness/corpus/artdeco_default.mp');
const displayListOom = source('tools/host_harness/wasm/fixtures/device_oom_displaylist.mp');
const seascape2Path = join(ROOT, 'examples/scripts/seascape2.mp');
const seascape2 = existsSync(seascape2Path) ? readFileSync(seascape2Path, 'utf8') : null;
const city2 = JSON.parse(source(
    'tools/server/backups/2026-08-27-s3/scripts/kksh2hjtkb/city-2-by-telohtrab.json'
)).content;

async function load(name, constrained) {
    const factory = (await import(join(HERE, 'out', `${name}.js`))).default;
    const M = await factory();
    const number = (symbol, args = []) => M.cwrap(symbol, 'number', args);
    const api = {
        M,
        constrained,
        render: number('mp_render',
            ['string', 'number', 'number', 'number', 'number', 'number', 'number']),
        pixels: number('mp_pixels'),
        error: number('mp_error'),
        items: number('mp_display_list_items'),
    };
    if (constrained) {
        api.setState = M.cwrap('mp_set_device_state', null, ['number']);
        api.compile = M.cwrap('mp_compile', 'number', ['string']);
        api.compileFileBytes = number('mp_compile_file_bytes');
        api.profile = M.cwrap('mp_device_profile', 'string', []);
        api.arduino = M.cwrap('mp_device_arduino', 'string', []);
        api.idf = M.cwrap('mp_device_idf', 'string', []);
        api.mem = {};
        for (const symbol of [
            'mp_device_profile_calibrated', 'mp_device_state_calibrated',
            'mp_mem_initial_internal_free', 'mp_mem_current_internal_free',
            'mp_mem_current_internal_largest', 'mp_mem_peak_internal_used',
            'mp_mem_failure_valid', 'mp_mem_failure_request',
            'mp_mem_failure_line',
            'mp_mem_failure_phase', 'mp_mem_failure_source',
            'mp_mem_failure_internal_free', 'mp_mem_failure_internal_largest',
        ]) api.mem[symbol] = number(symbol);
    }
    return api;
}

function render(api, script, width, height, seed = SEED, state = 0) {
    if (api.constrained) api.setState(state);
    const ok = !!api.render(script, width, height, seed.counter, seed.hour,
        seed.minute, seed.second);
    const out = {
        ok,
        error: api.M.UTF8ToString(api.error()),
        items: api.items(),
        pixels: null,
        memory: null,
    };
    if (ok) {
        const ptr = api.pixels();
        out.pixels = Uint8Array.from(
            api.M.HEAPU8.subarray(ptr, ptr + width * height));
    }
    if (api.constrained) {
        out.memory = Object.fromEntries(
            Object.entries(api.mem).map(([key, fn]) => [key, fn()]));
    }
    return out;
}

function samePixels(actual, expected, label) {
    assert.equal(actual.ok, true, `${label}: constrained render failed: ${actual.error}`);
    assert.equal(expected.ok, true, `${label}: reference render failed: ${expected.error}`);
    assert.deepEqual(actual.pixels, expected.pixels,
        `${label}: constrained pixels differ from the firmware oracle`);
    console.log(`SAME  ${label.padEnd(34)} items=${actual.items}`);
}

console.log('Constrained device WASM verification\n');

const reference = await load('mp_render', false);
const watchy = await load('mp_render_watchy', true);
const m5paper = await load('mp_render_m5paper', true);
assert.deepEqual([watchy.profile(), watchy.arduino(), watchy.idf()],
    ['watchy2', '3.1.3', '5.3.2']);
assert.deepEqual([m5paper.profile(), m5paper.arduino(), m5paper.idf()],
    ['m5paper', '2.0.4', '4.4.1']);
console.log('PASS  pinned toolchain metadata');

for (const [label, script] of [['Watchy prims', prims], ['Watchy Art Deco', artDeco]]) {
    samePixels(render(watchy, script, 200, 200),
        render(reference, script, 200, 200), label);
}
samePixels(render(m5paper, prims, 540, 960),
    render(reference, prims, 540, 960), 'M5Paper prims');

const fixtureOom = render(watchy, displayListOom, 200, 200);
assert.equal(fixtureOom.ok, false, 'Watchy display-list fixture should exceed the calibrated heap');
assert.equal(fixtureOom.memory.mp_mem_failure_phase, 3);
assert.equal(fixtureOom.memory.mp_mem_failure_source, 1);
assert.equal(fixtureOom.memory.mp_mem_failure_line, 6);
assert.ok(fixtureOom.memory.mp_mem_failure_internal_largest <
    fixtureOom.memory.mp_mem_failure_request);
console.log('OOM   Watchy tracked display-list fixture');

if (seascape2) {
    // Seascape II is the script that used to reboot the Watchy on a button
    // press: its parse tree (~146 KB of list nodes and Strings) did not fit
    // next to the BLE stack. It is now the biggest program in the corpus and
    // must go through the device's two stages cleanly:
    //   1. compile at sync, radios off  (mp_compile)
    //   2. load + render at wake, in ANY radio state (mp_render), never parsing
    const compileOk = watchy.compile(seascape2);
    const compileMem = Object.fromEntries(Object.entries(watchy.mem).map(([k, fn]) => [k, fn()]));
    assert.equal(compileOk, 1, 'Seascape 2 must compile within the radios-off Watchy heap');
    assert.equal(compileMem.mp_mem_failure_valid, 0);
    const compileBytes = watchy.compileFileBytes();
    assert.ok(compileBytes > 0 && compileBytes < 40000, `stored program should be a few tens of KB, got ${compileBytes}`);
    console.log(`OK    Watchy Seascape 2 compile        peak=${compileMem.mp_mem_peak_internal_used} stored=${compileBytes}B`);

    const expected = render(reference, seascape2, 200, 200);
    for (const [label, state] of [['radios off', 0], ['BLE active', 1]]) {
        const sea = render(watchy, seascape2, 200, 200, SEED, state);
        samePixels(sea, expected, `Watchy Seascape 2 (${label})`);
        assert.equal(sea.memory.mp_mem_failure_valid, 0);
        assert.ok(sea.memory.mp_mem_peak_internal_used < 80000,
            `render from stored program should stay well under the heap, got ${sea.memory.mp_mem_peak_internal_used}`);
        console.log(`      ${label.padEnd(12)} peak=${sea.memory.mp_mem_peak_internal_used}`);
    }
} else {
    console.log('SKIP  exact Seascape 2 fixture is not present');
}

const recovered = render(watchy, prims, 200, 200);
assert.equal(recovered.ok, true, 'Watchy must recover on the call after a simulated reboot');
assert.equal(recovered.memory.mp_mem_failure_valid, 0);
console.log('PASS  post-OOM Watchy reboot boundary');

const radioOff = render(watchy, prims, 200, 200, SEED, 0);
const ble = render(watchy, prims, 200, 200, SEED, 1);
const wifi = render(watchy, prims, 200, 200, SEED, 2);
const initial = (r) => r.memory.mp_mem_initial_internal_free;
assert.equal(initial(radioOff), 177476);
assert.equal(initial(ble), 87668);
assert.ok(initial(wifi) < initial(radioOff));
assert.equal(radioOff.memory.mp_device_profile_calibrated, 1);
assert.equal(radioOff.memory.mp_device_state_calibrated, 1);
assert.equal(ble.memory.mp_device_state_calibrated, 1);
assert.equal(wifi.memory.mp_device_state_calibrated, 0);
console.log(
    `PASS  Watchy states                      off=${initial(radioOff)} ` +
    `BLE=${initial(ble)} WiFi/TLS=${initial(wifi)}`);

// One M5 module is deliberately reused: this is the long-lived RenderTask
// lifecycle, not three independent cold-start measurements.
const m5First = render(m5paper, prims, 200, 200);
const m5Second = render(m5paper, artDeco, 200, 200);
assert.equal(m5First.ok, true);
assert.equal(m5Second.ok, true);
assert.ok(m5First.memory.mp_mem_current_internal_free <
    m5First.memory.mp_mem_initial_internal_free);
assert.ok(m5Second.memory.mp_mem_current_internal_free <
    m5Second.memory.mp_mem_initial_internal_free);
console.log('PASS  M5Paper persistent RenderTask allocations');

for (const state of [0, 1, 2]) {
    let successes = 0;
    let maxItems = 0;
    for (let counter = 0; counter < 60; counter++) {
        const sample = render(watchy, city2, 200, 200, {
            counter, hour: 12, minute: 34, second: (counter * 23) % 60,
        }, state);
        if (sample.ok) successes++;
        maxItems = Math.max(maxItems, sample.items);
    }
    assert.equal(successes, 60,
        `City 2 should use the low-memory occupancy fallback in state ${state}`);
    assert.ok(maxItems <= 37,
        `City 2 item count unexpectedly exceeded measured bound: ${maxItems}`);
    console.log(
        `PASS  City 2 state ${state} seed sweep          60/60; max items=${maxItems}`);
}

console.log('\nAll constrained profile checks passed.');
