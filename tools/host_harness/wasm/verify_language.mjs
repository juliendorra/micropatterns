#!/usr/bin/env node
// Explicit parser-language contract. Pixel goldens protect rendering semantics;
// these cases protect accepted syntax and important rejection boundaries.
import assert from 'node:assert/strict';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const factory = (await import(join(HERE, 'out', 'mp_render.js'))).default;
const M = await factory();
const parse = M.cwrap('mp_parse', 'number', ['string']);
const errorCount = M.cwrap('mp_parse_error_count', 'number', []);
const errorAt = M.cwrap('mp_parse_error_at', 'number', ['number']);

function errors() {
    return Array.from({ length: errorCount() }, (_, i) =>
        M.UTF8ToString(errorAt(i)));
}

function accepts(name, source) {
    assert.equal(parse(source), 1, `${name} should parse: ${errors().join('; ')}`);
    assert.deepEqual(errors(), [], `${name} produced parser errors`);
    console.log(`PASS  accepts ${name}`);
}

function rejects(name, source, expected) {
    assert.equal(parse(source), 0, `${name} should be rejected`);
    assert.ok(errors().some((message) => message.includes(expected)),
        `${name} did not report ${JSON.stringify(expected)}: ${errors().join('; ')}`);
    console.log(`PASS  rejects ${name}`);
}

accepts('complete case-insensitive command vocabulary', `
# Commands, parameters, variables, expressions, blocks, and quoted names are
# deliberately lower-case to lock in the language's case-insensitive contract.
define pattern name="checker-one" width=2 height=2 data="1001"
var $x = 1 + 2 * 3
let $x = $x % 5
color name=black
fill name="checker-one"
draw name="checker-one" x=0 y=0
reset_transforms
translate dx=1 dy=2
rotate degrees=90
scale factor=2
pixel x=0 y=0
fill_pixel x=1 y=1
line x1=0 y1=0 x2=2 y2=2
rect x=0 y=0 width=2 height=2
fill_rect x=0 y=0 width=2 height=2
circle x=1 y=1 radius=1
fill_circle x=1 y=1 radius=1
repeat count=2
  if $x == 2 then
    color name=white
  else
    fill name=solid
  endif
endrepeat
`);

accepts('environment variables and nested blocks', `
VAR $phase = $HOUR + $MINUTE + $SECOND + $COUNTER + $WIDTH + $HEIGHT
REPEAT COUNT=2
  IF $INDEX % 2 == 0 THEN
    PIXEL X=$INDEX Y=$phase
  ELSE
    FILL_PIXEL X=$INDEX Y=$phase
  ENDIF
ENDREPEAT
`);

rejects('unknown command', 'NOT_A_COMMAND X=1', 'Unknown command');
rejects('undefined variable', 'VAR $x = $missing + 1', 'Undefined variable');
rejects('case-insensitive duplicate variable', 'VAR $x = 1\nVAR $X = 2',
    'already declared');
rejects('environment variable redeclaration', 'VAR $WIDTH = 1',
    'environment variable');
rejects('assignment before declaration', 'LET $x = 1', 'undeclared variable');
rejects('invalid pattern data',
    'DEFINE PATTERN NAME=x WIDTH=2 HEIGHT=2 DATA="10x1"',
    "must contain only '0' or '1'");
rejects('malformed named parameter', 'PIXEL X 1 Y=2', "Missing '='");
rejects('unmatched ELSE', 'ELSE', 'Unexpected ELSE');
rejects('unclosed REPEAT', 'REPEAT COUNT=2\nPIXEL X=0 Y=0', 'Unclosed REPEAT');
rejects('malformed IF condition', 'IF $HOUR THEN\nPIXEL X=0 Y=0\nENDIF',
    'Invalid condition structure');

console.log('\nAll language-contract checks passed.');
