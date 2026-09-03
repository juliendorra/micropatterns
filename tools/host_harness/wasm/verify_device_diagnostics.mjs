#!/usr/bin/env node
import assert from 'node:assert/strict';
import {
    DEVICE_DIAGNOSTIC_SCHEMA_VERSION,
    diagnosticFromParseError,
    diagnosticsForRender,
} from '../../../micropatterns_emulator/device_diagnostics.js';

const source = 'COLOR NAME=BLACK\nREPEAT COUNT=1200\n  FILL_RECT X=$INDEX Y=0 WIDTH=1 HEIGHT=1';
const baseMemory = {
    constrained: true,
    profile: 'watchy2',
    deviceState: 1,
    stateCalibrated: true,
    internal: { initialFree: 87668, peakUsed: 76000, peakLine: 3 },
    psram: { initialFree: 0, peakUsed: 0, peakLine: 0 },
};

const oom = diagnosticsForRender({
    ok: false,
    error: 'device OOM: line=3 phase=3',
    source,
    memory: {
        ...baseMemory,
        failure: {
            line: 3, request: 10240, phase: 3, source: 1, capability: 0,
            internalFree: 13200, internalLargest: 7668,
            psramFree: 0, psramLargest: 0,
        },
    },
    stats: null,
});
assert.equal(oom.length, 1);
assert.equal(oom[0].schemaVersion, DEVICE_DIAGNOSTIC_SCHEMA_VERSION);
assert.equal(oom[0].code, 'MP_DEVICE_OOM');
assert.equal(oom[0].line, 3);
assert.equal(oom[0].sourceText, 'FILL_RECT X=$INDEX Y=0 WIDTH=1 HEIGHT=1');
assert.match(oom[0].explanation, /fragmentation/);
console.log('PASS  structured OOM points to the executing source line');

const compileOom = diagnosticsForRender({
    ok: false,
    stage: 'compile',
    error: 'device OOM at sync (compile): line=3 phase=2',
    source,
    memory: {
        ...baseMemory,
        deviceState: 0,
        failure: {
            line: 3, request: 4096, phase: 2, source: 1, capability: 0,
            internalFree: 2048, internalLargest: 1024,
            psramFree: 0, psramLargest: 0,
        },
    },
    stats: null,
});
assert.equal(compileOom[0].stage, 'compile');
assert.match(compileOom[0].title, /reject.*during sync/i);
assert.match(compileOom[0].message, /compiling or loading the program/);
assert.match(compileOom[0].suggestions.join(' '), /not a wake-time reboot/);
console.log('PASS  compile OOM is explained as a radios-off sync failure');

const parser = diagnosticFromParseError({
    line: 2, message: 'Unknown command: WAT', raw: 'Line 2: Unknown command: WAT',
}, 'COLOR NAME=BLACK\nWAT');
assert.equal(parser.code, 'MP_PARSE_ERROR');
assert.equal(parser.line, 2);
assert.equal(parser.sourceText, 'WAT');
console.log('PASS  parser diagnostics use the same editor contract');

const warnings = diagnosticsForRender({
    ok: true,
    error: null,
    source,
    memory: { ...baseMemory, failure: null },
    stats: {
        occupancyMapUsed: false, totalItems: 2000, renderedItems: 1900,
        culledOffScreen: 100, displayListBytes: 80000,
    },
});
assert.deepEqual(warnings.map((d) => d.code),
    ['MP_OCCUPANCY_FALLBACK', 'MP_MEMORY_PRESSURE']);
assert.equal(warnings[1].line, 3);
console.log('PASS  successful runs surface fallback and memory pressure');

const generic = diagnosticsForRender({
    ok: false, error: 'Runtime Error (Line 2): division by zero', source,
    memory: { constrained: false }, stats: null,
});
assert.equal(generic[0].code, 'MP_RUNTIME_ERROR');
assert.equal(generic[0].line, 2);
console.log('PASS  unstructured future runtime errors degrade gracefully');

console.log('\nAll editor diagnostic-contract checks passed.');
