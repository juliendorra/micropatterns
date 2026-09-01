// 32-bit integer arithmetic, matching the firmware.
//
// MicroPatterns runs on a 32-bit microcontroller and its C++ runtime evaluates
// every expression as `int` (micropatterns_runtime.cpp,
// evaluateExpressionRange). JavaScript numbers are doubles, so the two engines
// agree right up until a script overflows -- and then they disagree silently,
// with no error on either side.
//
// Measured with `VAR $a = 100000 / LET $a = $a * 100000 / LET $a = $a % 97`:
//
//     device (int32 wrap)  ->  1410065408 % 97  =  76
//     web    (double)      ->  10000000000 % 97 =  49
//
// and a FILL_RECT seeded from it landed 27 pixels apart. Scripts that seed a
// pseudo-random sequence from $SECOND/$COUNTER do exactly this kind of
// multiplication, so it is not a corner case: it is how a script that looks
// right in the editor renders differently on the watch.
//
// The device is the reference here -- it is the target the language is for --
// so the web emulates its width rather than the other way round.
//
// Signed overflow is technically undefined in C++; in practice every compiler
// targeting these devices wraps, which is what these helpers reproduce.

export const i32 = (v) => v | 0;
export const i32add = (a, b) => (a + b) | 0;
export const i32sub = (a, b) => (a - b) | 0;

// Math.imul is the only way to get a correct 32-bit product in JS: `a * b | 0`
// is wrong for large operands because the double product loses low bits before
// the truncation happens.
export const i32mul = (a, b) => Math.imul(a, b);

// C++ integer division truncates toward zero, which is what Math.trunc does.
export const i32div = (a, b) => Math.trunc(a / b) | 0;

// C++ % also truncates toward zero, and so does JS %, so the signs already
// agree; the |0 only re-narrows the result.
export const i32mod = (a, b) => (a % b) | 0;
