#include "micropatterns_runtime.h"
#include "esp32-hal-log.h"
#include <Arduino.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "mp_wdt.h"
#include "mp_diagnostics.h"
#include "matrix_utils.h"

const uint8_t RUNTIME_COLOR_WHITE = 0;
const uint8_t RUNTIME_COLOR_BLACK = 15;

// Identity transform every DisplayListItem points at until the runtime gives it
// a pooled snapshot. Declared in micropatterns_command.h.
const TransformSnapshot kIdentityTransform;

// MicroPatterns arithmetic is explicitly signed 32-bit with two's-complement
// wrap, matching both ESP32 targets. Performing overflowing operations on a
// C++ `int` is undefined behavior and lets host/WASM optimizers drift from the
// devices, so do the operation in uint32_t and preserve its resulting bits.
static inline int32_t wrapI32(uint32_t bits) {
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static inline int32_t addI32(int32_t l, int32_t r) { return wrapI32((uint32_t)l + (uint32_t)r); }
static inline int32_t subI32(int32_t l, int32_t r) { return wrapI32((uint32_t)l - (uint32_t)r); }
static inline int32_t mulI32(int32_t l, int32_t r) { return wrapI32((uint32_t)l * (uint32_t)r); }

MicroPatternsRuntime::MicroPatternsRuntime(int canvasWidth, int canvasHeight, const MpProgram& program)
    : _program(program), _interrupt_requested(false), _interrupt_check_cb(nullptr),
      _canvasWidth(canvasWidth), _canvasHeight(canvasHeight) {
    _vals.assign(MP_ENV_SLOT_COUNT + program.userVariableCount(), 0);
    _defined.assign(program.userVariableCount(), 0);
    _vals[MP_ENV_WIDTH] = _canvasWidth;
    _vals[MP_ENV_HEIGHT] = _canvasHeight;
    _stack.resize(program.maxExprStack > 0 ? program.maxExprStack : 1);
    _loops.reserve(8);
    resetStateAndList();
}

int MicroPatternsRuntime::getCounter() const { return _vals[MP_ENV_COUNTER]; }

void MicroPatternsRuntime::getTime(int& hour, int& minute, int& second) const {
    hour = _vals[MP_ENV_HOUR];
    minute = _vals[MP_ENV_MINUTE];
    second = _vals[MP_ENV_SECOND];
}

void MicroPatternsRuntime::setCounter(int counter) { _vals[MP_ENV_COUNTER] = counter; }

void MicroPatternsRuntime::setTime(int hour, int minute, int second) {
    _vals[MP_ENV_HOUR] = hour;
    _vals[MP_ENV_MINUTE] = minute;
    _vals[MP_ENV_SECOND] = second;
}

void MicroPatternsRuntime::resetStateAndList() {
    _currentState = MicroPatternsState();
    for (size_t i = 0; i < _defined.size(); ++i) {
        _defined[i] = 0;
        _vals[MP_ENV_SLOT_COUNT + i] = 0;
    }
    _vals[MP_ENV_INDEX] = 0;
    _loops.clear();
    _displayList.clear();
    _xfPool.clear();
    _xfDirty = true;
    _executed = 0;
}

void MicroPatternsRuntime::runtimeError(const String& message, int lineNumber) {
    log_e("Runtime Error (Line %d): %s", lineNumber, message.c_str());
}

const TransformSnapshot* MicroPatternsRuntime::currentTransform() {
    if (_xfDirty || _xfPool.empty()) {
        TransformSnapshot snap;
        memcpy(snap.matrix, _currentState.matrix, sizeof(float) * 6);
        memcpy(snap.inverseMatrix, _currentState.inverseMatrix, sizeof(float) * 6);
        snap.scale = _currentState.scale;
        // A loop body that does RESET_TRANSFORMS / SCALE / TRANSLATE with the
        // same arguments every iteration marks the state dirty every iteration
        // but recomputes the same numbers, so compare against the last snapshot
        // before appending. Only a bitwise-identical snapshot is reused, so the
        // floats every item sees are exactly the ones it would have got.
        if (!_xfPool.empty() &&
            memcmp(&_xfPool.back(), &snap, sizeof(TransformSnapshot)) == 0) {
            _xfDirty = false;
            return &_xfPool.back();
        }
        _xfPool.push_back(snap);
        _xfDirty = false;
    }
    return &_xfPool.back();
}

// One operand -> value. Slots were range-checked when the program was built or
// deserialized, so the hot path is a load and, for user slots, a defined check.
int32_t MicroPatternsRuntime::resolve(const MpOperand& o, int line) {
    if (o.kind == MP_OPND_LIT) return o.v;
    const int32_t slot = o.v;
    if (slot < MP_ENV_SLOT_COUNT) {
        if (slot == MP_ENV_INDEX && _loops.empty()) {
            runtimeError("Variable $INDEX can only be used inside a REPEAT loop.", line);
            return 0;
        }
        return _vals[slot];
    }
    const size_t user = (size_t)(slot - MP_ENV_SLOT_COUNT);
    if (_defined[user]) return _vals[slot];
    runtimeError("Undefined variable: $" + _program.varNames[user], line);
    return 0;
}

// Postfix evaluation on a preallocated stack. Division/modulo by zero abort the
// whole expression with 0 and an error, exactly as the old two-pass evaluator
// did; INT_MIN / -1 and INT_MIN % -1 are defined rather than left to the CPU.
int32_t MicroPatternsRuntime::eval(int32_t begin, int32_t len, int line) {
    if (len <= 0) return 0;
    const MpOperand* e = _program.exprs.data() + begin;
    int32_t* st = _stack.data();
    int sp = 0;
    for (int32_t i = 0; i < len; ++i) {
        const MpOperand& o = e[i];
        if (o.kind != MP_OPND_OP) {
            st[sp++] = resolve(o, line);
            continue;
        }
        if (sp < 2) { runtimeError("Syntax error in expression.", line); return 0; }
        const int32_t r = st[--sp];
        const int32_t l = st[sp - 1];
        int32_t v;
        switch (o.op) {
            case MP_OP_ADD: v = addI32(l, r); break;
            case MP_OP_SUB: v = subI32(l, r); break;
            case MP_OP_MUL: v = mulI32(l, r); break;
            case MP_OP_DIV:
                if (r == 0) { runtimeError("Division by zero.", line); return 0; }
                v = (l == INT_MIN && r == -1) ? INT_MIN : l / r;
                break;
            case MP_OP_MOD:
                if (r == 0) { runtimeError("Modulo by zero.", line); return 0; }
                v = (l == INT_MIN && r == -1) ? 0 : l % r;
                break;
            case MP_OP_EQ: v = (l == r); break;
            case MP_OP_NE: v = (l != r); break;
            case MP_OP_LT: v = (l <  r); break;
            case MP_OP_LE: v = (l <= r); break;
            case MP_OP_GT: v = (l >  r); break;
            case MP_OP_GE: v = (l >= r); break;
            default: runtimeError("Unknown operator.", line); return 0;
        }
        st[sp - 1] = v;
    }
    return sp > 0 ? st[sp - 1] : 0;
}

bool MicroPatternsRuntime::evalCondition(const MpInstr& in) {
    if (in.aux) {
        // The parser found no single comparison with two operands. The old
        // runtime reported that here and took the branch as false.
        runtimeError("No comparison operator in condition.", in.line);
        return false;
    }
    return eval(in.x1, in.x2, in.line) != 0;
}

bool MicroPatternsRuntime::isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const {
    if (!asset || asset->data.empty()) return false;
    for (uint8_t pixelValue : asset->data) {
        if (pixelValue == 0) return false; // 0 is transparent for DRAW
    }
    return true;
}

// ---------------------------------------------------------------------------
// The interpreter loop
// ---------------------------------------------------------------------------
void MicroPatternsRuntime::generateDisplayList() {
    mp_diagnostic_source_line(0);
    resetStateAndList();
    mp_wdt_reset();
    clearInterrupt();

    const std::vector<MpInstr>& code = _program.code;
    const int32_t n = (int32_t)code.size();
    const std::vector<MicroPatternsAsset>& assets = _program.assets;
    int32_t pc = 0;

    while (pc < n) {
        const MpInstr& in = code[pc];
        ++_executed;

        // Cooperative scheduling and abort, at a cadence no coarser than the
        // old per-command checks: every 16 instructions poll the interrupt
        // callback and yield, every 256 pet the watchdog.
        if ((_executed & 15u) == 0) {
            if (_interrupt_requested || (_interrupt_check_cb && _interrupt_check_cb())) {
                _interrupt_requested = true;
                break;
            }
            yield();
            if ((_executed & 255u) == 0) mp_wdt_reset();
        }
        mp_diagnostic_source_line(in.line);

        switch (in.type) {
            case CMD_VAR: {
                const int32_t v = eval(in.x1, in.x2, in.line);
                _vals[in.x0] = v;
                _defined[in.x0 - MP_ENV_SLOT_COUNT] = 1;
                ++pc;
                continue;
            }
            case CMD_LET: {
                const size_t user = (size_t)(in.x0 - MP_ENV_SLOT_COUNT);
                if (_defined[user]) {
                    _vals[in.x0] = eval(in.x1, in.x2, in.line);
                } else {
                    runtimeError("LET: Undeclared variable: $" + _program.varNames[user], in.line);
                }
                ++pc;
                continue;
            }
            case CMD_REPEAT: {
                const int32_t count = resolve(in.op[0], in.line);
                if (count < 0) {
                    runtimeError("REPEAT count negative.", in.line);
                    pc = in.x0;
                    continue;
                }
                if (count == 0) { pc = in.x0; continue; }
                _loops.push_back(Loop{pc + 1, count, 0});
                _vals[MP_ENV_INDEX] = 0;
                ++pc;
                continue;
            }
            case CMD_ENDREPEAT: {
                // The parser guarantees a matching REPEAT, but a REPEAT whose
                // count was <= 0 jumped past us, so the stack can only be
                // non-empty here.
                if (_loops.empty()) { ++pc; continue; }
                Loop& top = _loops.back();
                if (++top.i < top.count) {
                    _vals[MP_ENV_INDEX] = top.i;
                    pc = top.bodyPc;
                } else {
                    _loops.pop_back();
                    _vals[MP_ENV_INDEX] = _loops.empty() ? 0 : _loops.back().i;
                    ++pc;
                }
                continue;
            }
            case CMD_IF:
                pc = evalCondition(in) ? pc + 1 : in.x0;
                continue;
            case CMD_ELSE:
                pc = in.x0; // reached from the THEN branch: skip the ELSE part
                continue;
            case CMD_ENDIF:
                ++pc;
                continue;

            case CMD_COLOR:
                _currentState.color = in.aux;
                ++pc;
                continue;
            case CMD_FILL:
                if (in.aux == MP_ASSET_SOLID) {
                    _currentState.fillAsset = nullptr;
                } else if (in.aux == MP_ASSET_UNKNOWN) {
                    _currentState.fillAsset = nullptr;
                    runtimeError("Undefined fill pattern.", in.line);
                } else {
                    _currentState.fillAsset = &assets[in.aux];
                }
                ++pc;
                continue;
            case CMD_RESET_TRANSFORMS:
                _currentState.scale = 1.0f;
                matrix_identity(_currentState.matrix);
                matrix_identity(_currentState.inverseMatrix);
                _xfDirty = true;
                ++pc;
                continue;
            case CMD_TRANSLATE: {
                const float dx = (float)resolve(in.op[0], in.line);
                const float dy = (float)resolve(in.op[1], in.line);
                float T_op[6]; matrix_make_translation(T_op, dx, dy);
                matrix_multiply(_currentState.matrix, _currentState.matrix, T_op);
                if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
                _xfDirty = true;
                ++pc;
                continue;
            }
            case CMD_ROTATE: {
                const float degrees = (float)resolve(in.op[0], in.line);
                float R_op[6]; matrix_make_rotation(R_op, degrees);
                matrix_multiply(_currentState.matrix, _currentState.matrix, R_op);
                if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
                _xfDirty = true;
                ++pc;
                continue;
            }
            case CMD_SCALE: {
                const int32_t f = resolve(in.op[0], in.line);
                _currentState.scale = (float)(f < 1 ? 1 : f);
                _xfDirty = true;
                ++pc;
                continue;
            }

            case CMD_PIXEL: case CMD_FILL_PIXEL:
            case CMD_LINE:
            case CMD_RECT: case CMD_FILL_RECT:
            case CMD_CIRCLE: case CMD_FILL_CIRCLE:
            case CMD_DRAW:
                break; // drawing: build an item below

            default:
                ++pc;
                continue;
        }

        DisplayListItem item;
        item.type = (CommandType)in.type;
        item.sourceLine = in.line;
        item.xf = currentTransform();
        item.color = _currentState.color;
        item.fillAsset = _currentState.fillAsset;

        switch (in.type) {
            case CMD_PIXEL: case CMD_FILL_PIXEL:
                item.p[0] = resolve(in.op[0], in.line);
                item.p[1] = resolve(in.op[1], in.line);
                item.isOpaque = true;
                break;
            case CMD_LINE:
                item.p[0] = resolve(in.op[0], in.line);
                item.p[1] = resolve(in.op[1], in.line);
                item.p[2] = resolve(in.op[2], in.line);
                item.p[3] = resolve(in.op[3], in.line);
                break;
            case CMD_RECT: case CMD_FILL_RECT:
                item.p[0] = resolve(in.op[0], in.line);
                item.p[1] = resolve(in.op[1], in.line);
                item.p[2] = resolve(in.op[2], in.line);
                item.p[3] = resolve(in.op[3], in.line);
                item.isOpaque = (in.type == CMD_FILL_RECT);
                break;
            case CMD_CIRCLE: case CMD_FILL_CIRCLE:
                item.p[0] = resolve(in.op[0], in.line);
                item.p[1] = resolve(in.op[1], in.line);
                item.p[2] = resolve(in.op[2], in.line);
                item.isOpaque = (in.type == CMD_FILL_CIRCLE);
                break;
            case CMD_DRAW:
                item.p[0] = resolve(in.op[0], in.line);
                item.p[1] = resolve(in.op[1], in.line);
                if (in.aux == MP_ASSET_SOLID || in.aux == MP_ASSET_UNKNOWN) {
                    runtimeError("DRAW: Invalid asset name.", in.line);
                    ++pc;
                    continue; // never added, as before
                }
                item.asset = &assets[in.aux];
                item.isOpaque = isAssetDataFullyOpaque(item.asset);
                break;
            default:
                break;
        }
        _displayList.push_back(item);
        ++pc;
    }
    mp_wdt_reset();
}
