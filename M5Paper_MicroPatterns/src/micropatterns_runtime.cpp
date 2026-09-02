#include "micropatterns_runtime.h"
#include "esp32-hal-log.h"
#include <Arduino.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include "mp_wdt.h"
#include "matrix_utils.h"

const uint8_t RUNTIME_COLOR_WHITE = 0;
const uint8_t RUNTIME_COLOR_BLACK = 15;

// Identity transform every DisplayListItem points at until the runtime gives it
// a pooled snapshot. Declared in micropatterns_command.h.
const TransformSnapshot kIdentityTransform;

// Epoch source for the slot memos held in ParamValue / MicroPatternsCommand.
// Each runtime instance takes a fresh value so it can never read a memo another
// instance wrote against a different slot table. Starts at 1: the default memo
// epoch is 0, which must never match.
static uint32_t s_runtimeEpochCounter = 0;

// MicroPatterns arithmetic is explicitly signed 32-bit with two's-complement
// wrap, matching both ESP32 targets. Performing overflowing operations on a
// C++ `int` is undefined behavior and lets host/WASM optimizers drift from the
// devices, so do the operation in uint32_t and preserve its resulting bits.
static int wrapI32(uint32_t bits) {
    int32_t value;
    static_assert(sizeof(value) == sizeof(bits), "32-bit arithmetic required");
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int addI32(int left, int right) {
    return wrapI32((uint32_t)left + (uint32_t)right);
}

static int subI32(int left, int right) {
    return wrapI32((uint32_t)left - (uint32_t)right);
}

static int mulI32(int left, int right) {
    return wrapI32((uint32_t)left * (uint32_t)right);
}

MicroPatternsRuntime::MicroPatternsRuntime(int canvasWidth, int canvasHeight, const std::map<String, MicroPatternsAsset>& assets)
    : _assets(assets), _interrupt_requested(false), _interrupt_check_cb(nullptr),
      _canvasWidth(canvasWidth), _canvasHeight(canvasHeight) {
    _epoch = ++s_runtimeEpochCounter;
    if (_epoch == 0) _epoch = ++s_runtimeEpochCounter; // never hand out 0

    for (int i = 0; i < ENV_SLOT_COUNT; ++i) _envValues[i] = 0;
    _envValues[ENV_WIDTH] = _canvasWidth;
    _envValues[ENV_HEIGHT] = _canvasHeight;

    seedEnvSlots();

    resetStateAndList();
}

// Seeds the fixed env slots so internSlot() resolves them without any
// special-casing at lookup time.
void MicroPatternsRuntime::seedEnvSlots() {
    _slotByName["$WIDTH"] = ENV_WIDTH;
    _slotByName["$HEIGHT"] = ENV_HEIGHT;
    _slotByName["$HOUR"] = ENV_HOUR;
    _slotByName["$MINUTE"] = ENV_MINUTE;
    _slotByName["$SECOND"] = ENV_SECOND;
    _slotByName["$COUNTER"] = ENV_COUNTER;
    _slotByName["$INDEX"] = ENV_INDEX;
}

int MicroPatternsRuntime::getCounter() const {
    return _envValues[ENV_COUNTER];
}

void MicroPatternsRuntime::getTime(int& hour, int& minute, int& second) const {
    hour = _envValues[ENV_HOUR];
    minute = _envValues[ENV_MINUTE];
    second = _envValues[ENV_SECOND];
}

void MicroPatternsRuntime::setCommands(const std::list<MicroPatternsCommand>* commands) {
    // A new command list means the slot/asset memos held in the OLD parse tree
    // must never be honoured again -- and, more importantly, that this runtime's
    // slot table no longer describes the incoming one. Taking a fresh epoch
    // here makes every memo in the new tree miss on first use and re-intern,
    // which is what the "one epoch per runtime instance" scheme silently
    // assumed but did not enforce. Cold path: once per render.
    if (commands != _commands) {
        _epoch = ++s_runtimeEpochCounter;
        if (_epoch == 0) _epoch = ++s_runtimeEpochCounter;
        _slotByName.clear();
        seedEnvSlots();
        _varValues.clear();
        _varDefined.clear();
    }
    _commands = commands;
}

void MicroPatternsRuntime::setDeclaredVariables(const std::set<String>* declaredVariables) {
    _declaredVariables = declaredVariables;
}

void MicroPatternsRuntime::resetStateAndList() {
    _currentState = MicroPatternsState();
    // Slot names survive across renders (they belong to the script, not the
    // run); only the values and their defined-ness reset.
    for (size_t i = 0; i < _varDefined.size(); ++i) {
        _varDefined[i] = 0;
        _varValues[i] = 0;
    }
    _envValues[ENV_INDEX] = 0;
    _displayList.clear();
    _xfPool.clear();
    _xfDirty = true;
}

void MicroPatternsRuntime::setCounter(int counter) {
    _envValues[ENV_COUNTER] = counter;
}

void MicroPatternsRuntime::setTime(int hour, int minute, int second) {
    _envValues[ENV_HOUR] = hour;
    _envValues[ENV_MINUTE] = minute;
    _envValues[ENV_SECOND] = second;
}

void MicroPatternsRuntime::runtimeError(const String& message, int lineNumber) {
    log_e("Runtime Error (Line %d): %s", lineNumber, message.c_str());
}

int MicroPatternsRuntime::internSlot(const String& nameWithDollar) {
    auto it = _slotByName.find(nameWithDollar);
    if (it != _slotByName.end()) return it->second;
    int slot = ENV_SLOT_COUNT + (int)_varValues.size();
    _varValues.push_back(0);
    _varDefined.push_back(0);
    _slotByName[nameWithDollar] = slot;
    return slot;
}

int MicroPatternsRuntime::internSlotForToken(const ParamValue& val) {
    String upperName = val.stringValue;
    upperName.toUpperCase();
    int slot = internSlot(upperName);
    val.slotCache = slot;
    val.slotEpoch = _epoch;
    return slot;
}

int MicroPatternsRuntime::slotForCommandTarget(const MicroPatternsCommand& cmd, const String& bareUpperName) {
    if (cmd.targetSlotEpoch == _epoch) return cmd.targetSlotCache;
    int slot = internSlot("$" + bareUpperName);
    cmd.targetSlotCache = slot;
    cmd.targetSlotEpoch = _epoch;
    return slot;
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

int MicroPatternsRuntime::resolveValue(const ParamValue& val, int lineNumber, int loopIndex) {
    if (val.type == ParamValue::TYPE_INT) {
        return val.intValue;
    } else if (val.type == ParamValue::TYPE_VARIABLE) {
        int slot = slotForVariableToken(val);
        if (slot == ENV_INDEX) {
            if (loopIndex < 0) {
                runtimeError("Variable $INDEX can only be used inside a REPEAT loop.", lineNumber);
                return 0;
            }
            return loopIndex;
        }
        if (slot >= 0 && slot < ENV_SLOT_COUNT) return _envValues[slot];
        // Bounds check, not decoration. `slot` arrives from a memo written into
        // the (mutable) parse tree, so a memo that outlives the slot table it
        // was computed against -- a runtime handed a different command list, or
        // an epoch collision -- would index this vector out of range and
        // silently corrupt the heap. One compare per variable read; unmeasurable
        // next to the map lookup it replaced.
        size_t userSlot = (size_t)(slot - ENV_SLOT_COUNT);
        if (slot < 0 || userSlot >= _varValues.size()) {
            runtimeError("Internal: variable slot out of range for " + val.stringValue, lineNumber);
            return 0;
        }
        if (_varDefined[userSlot]) return _varValues[userSlot];
        runtimeError("Undefined variable: " + val.stringValue, lineNumber);
        return 0;
    }
    runtimeError("Expected integer or variable, got: " + val.stringValue, lineNumber);
    return 0;
}

const ParamValue* MicroPatternsRuntime::findParam(const std::map<String, ParamValue>& params, const char* name) {
    for (const auto& kv : params) {
        // NOT redundant on device. ESP32's WString returns its raw buffer from
        // c_str(), and String::init() sets that buffer to nullptr -- so an empty
        // String yields a NULL c_str() and strcmp() would dereference it. The
        // host shim is std::string-backed and never returns NULL, which is
        // exactly why this class of bug cannot be seen without the check (see
        // MP_SHIM_NULL_CSTR in the harness shim).
        const char* key = kv.first.c_str();
        if (key && strcmp(key, name) == 0) return &kv.second;
    }
    return nullptr;
}

int MicroPatternsRuntime::resolveIntParam(const char* paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber, int loopIndex) {
    const ParamValue* val = findParam(params, paramName);
    if (val) {
        if (val->type == ParamValue::TYPE_INT || val->type == ParamValue::TYPE_VARIABLE) {
            return resolveValue(*val, lineNumber, loopIndex);
        }
        runtimeError(String("Parameter ") + paramName + " requires an integer or variable.", lineNumber);
    }
    return defaultValue;
}

String MicroPatternsRuntime::resolveStringParam(const char* paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber) {
    const ParamValue* val = findParam(params, paramName);
    if (val) {
        if (val->type == ParamValue::TYPE_STRING) {
            return val->stringValue;
        }
        runtimeError(String("Parameter ") + paramName + " requires a string/keyword.", lineNumber);
    }
    return defaultValue;
}

String MicroPatternsRuntime::resolveAssetNameParam(const char* paramName, const std::map<String, ParamValue>& params, int lineNumber) {
    const ParamValue* val = findParam(params, paramName);
    if (val) {
        if (val->type == ParamValue::TYPE_STRING) {
            String nameValue = val->stringValue;
            nameValue.toUpperCase();
            return nameValue; // SOLID or UPPERCASE pattern name
        }
        runtimeError(String("Parameter ") + paramName + " requires SOLID or a pattern name string.", lineNumber);
    }
    return "SOLID"; // Default
}

uint8_t MicroPatternsRuntime::resolveAssetForCommand(const MicroPatternsCommand& cmd, const MicroPatternsAsset*& outAsset) {
    if (cmd.assetEpoch != _epoch) {
        String assetName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber);
        if (assetName == "SOLID") {
            cmd.assetKind = 1;
            cmd.assetCache = nullptr;
        } else {
            auto it = _assets.find(assetName);
            if (it == _assets.end()) {
                cmd.assetKind = 3;
                cmd.assetCache = nullptr;
            } else {
                cmd.assetKind = 2;
                cmd.assetCache = &it->second;
            }
        }
        cmd.assetEpoch = _epoch;
    }
    outAsset = cmd.assetCache;
    return cmd.assetKind;
}

int MicroPatternsRuntime::evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex) {
    return evaluateExpressionRange(tokens, 0, tokens.size(), lineNumber, loopIndex);
}

int MicroPatternsRuntime::evaluateExpressionRange(const std::vector<ParamValue>& tokens, size_t begin, size_t end,
                                                  int lineNumber, int loopIndex) {
    if (begin >= end) return 0;

    // Fast path for the overwhelmingly common single-token expression
    // (`VAR $x = 3`, `RECT X=$col ...`): no scratch buffer, no operator passes.
    if (end - begin == 1) {
        const ParamValue& only = tokens[begin];
        if (only.type == ParamValue::TYPE_INT) return only.intValue;
        if (only.type == ParamValue::TYPE_VARIABLE) return resolveValue(only, lineNumber, loopIndex);
        // A lone operator / string is a malformed expression; the general path
        // below also yields 0 for it, silently, so stay silent here too.
        return 0;
    }

    // Pass 0: resolve variables to values, keeping operators as pointers into
    // the (immutable) token vector. No String is copied and no ParamValue is
    // constructed, which is what the old resolvedTokens vector cost per call.
    std::vector<EvalTok>& resolved = _evalScratchA;
    resolved.clear();
    for (size_t i = begin; i < end; ++i) {
        const ParamValue& token = tokens[i];
        if (token.type == ParamValue::TYPE_VARIABLE) {
            resolved.push_back(EvalTok{resolveValue(token, lineNumber, loopIndex), nullptr});
        } else if (token.type == ParamValue::TYPE_INT) {
            resolved.push_back(EvalTok{token.intValue, nullptr});
        } else if (token.type == ParamValue::TYPE_OPERATOR) {
            resolved.push_back(EvalTok{0, &token});
        } else {
            runtimeError("Unexpected token type in expression: " + token.stringValue, lineNumber);
            return 0;
        }
    }

    if (resolved.empty()) return 0;
    if (resolved.back().op != nullptr) { /* Error: trailing operator */ return 0; }
    if (resolved.front().op != nullptr) { /* Error: leading operator */ return 0; }

    // Pass 1: multiplicative operators (*, /, %)
    std::vector<EvalTok>& pass1Result = _evalScratchB;
    pass1Result.clear();
    for (size_t i = 0; i < resolved.size(); ++i) {
        const EvalTok& currentToken = resolved[i];
        const char* opStr = currentToken.op ? currentToken.op->stringValue.c_str() : nullptr;
        if (opStr && opStr[1] == '\0' && (opStr[0] == '*' || opStr[0] == '/' || opStr[0] == '%')) {
            if (pass1Result.empty() || pass1Result.back().op != nullptr ||
                i + 1 >= resolved.size() || resolved[i + 1].op != nullptr) {
                runtimeError("Syntax error with operator " + currentToken.op->stringValue, lineNumber); return 0;
            }
            int leftVal = pass1Result.back().value; pass1Result.pop_back();
            int rightVal = resolved[i + 1].value;
            int result = 0;
            if (opStr[0] == '*') {
                result = mulI32(leftVal, rightVal);
            } else if (opStr[0] == '/') {
                if (rightVal == 0) { runtimeError("Division by zero.", lineNumber); return 0; }
                // INT_MIN / -1 is the only overflowing signed division. Its
                // 32-bit wrapped result is INT_MIN; define it rather than rely
                // on compiler/CPU-specific overflow behavior.
                result = (leftVal == INT_MIN && rightVal == -1)
                    ? INT_MIN : leftVal / rightVal;
            } else {
                if (rightVal == 0) { runtimeError("Modulo by zero.", lineNumber); return 0; }
                result = (leftVal == INT_MIN && rightVal == -1)
                    ? 0 : leftVal % rightVal;
            }
            pass1Result.push_back(EvalTok{result, nullptr});
            i++; // Skip right operand
        } else {
            pass1Result.push_back(currentToken);
        }
    }

    if (pass1Result.empty()) return 0; // Should not happen if resolved was not empty
    int finalResult = pass1Result[0].value; // Must start with a number
    for (size_t i = 1; i < pass1Result.size(); i += 2) {
        if (i + 1 >= pass1Result.size() || pass1Result[i].op == nullptr || pass1Result[i + 1].op != nullptr) {
            runtimeError("Syntax error in expression (AS pass).", lineNumber); return 0;
        }
        const char* op = pass1Result[i].op->stringValue.c_str();
        int rightVal = pass1Result[i + 1].value;
        if (op[0] == '+' && op[1] == '\0') finalResult = addI32(finalResult, rightVal);
        else if (op[0] == '-' && op[1] == '\0') finalResult = subI32(finalResult, rightVal);
        else { runtimeError("Unexpected operator in AS pass: " + pass1Result[i].op->stringValue, lineNumber); return 0; }
    }
    return finalResult;
}

bool MicroPatternsRuntime::evaluateCondition(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex) {
    if (tokens.empty()) { runtimeError("Empty condition.", lineNumber); return false; }

    int comparisonOpIndex = -1;
    const char* comparisonOp = nullptr;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == ParamValue::TYPE_OPERATOR) {
            const char* opStr = tokens[i].stringValue.c_str();
            bool isComparison =
                (opStr[0] == '=' && opStr[1] == '=' && opStr[2] == '\0') ||
                (opStr[0] == '!' && opStr[1] == '=' && opStr[2] == '\0') ||
                (opStr[0] == '<' && (opStr[1] == '\0' || (opStr[1] == '=' && opStr[2] == '\0'))) ||
                (opStr[0] == '>' && (opStr[1] == '\0' || (opStr[1] == '=' && opStr[2] == '\0')));
            if (isComparison) {
                if (comparisonOpIndex != -1) { runtimeError("Multiple comparison operators.", lineNumber); return false; }
                comparisonOpIndex = i; comparisonOp = opStr;
            }
        }
    }
    if (comparisonOpIndex == -1) { runtimeError("No comparison operator in condition.", lineNumber); return false; }

    // Operand ranges, not copied sub-vectors.
    size_t leftBegin = 0, leftEnd = (size_t)comparisonOpIndex;
    size_t rightBegin = (size_t)comparisonOpIndex + 1, rightEnd = tokens.size();
    if (leftBegin >= leftEnd || rightBegin >= rightEnd) { runtimeError("Missing operand in condition.", lineNumber); return false; }

    int leftValue = evaluateExpressionRange(tokens, leftBegin, leftEnd, lineNumber, loopIndex);
    int rightValue = evaluateExpressionRange(tokens, rightBegin, rightEnd, lineNumber, loopIndex);

    if (comparisonOp[0] == '=') return leftValue == rightValue;
    if (comparisonOp[0] == '!') return leftValue != rightValue;
    if (comparisonOp[0] == '<') return comparisonOp[1] == '=' ? (leftValue <= rightValue) : (leftValue < rightValue);
    if (comparisonOp[0] == '>') return comparisonOp[1] == '=' ? (leftValue >= rightValue) : (leftValue > rightValue);
    runtimeError(String("Unknown comparison operator: ") + comparisonOp, lineNumber);
    return false;
}

bool MicroPatternsRuntime::isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const {
    if (!asset || asset->data.empty()) return false;
    for (uint8_t pixelValue : asset->data) {
        if (pixelValue == 0) return false; // 0 is transparent for DRAW
    }
    return true;
}


void MicroPatternsRuntime::generateDisplayList() {
    if (!_commands || !_declaredVariables) {
        log_e("Runtime not properly initialized for display list generation.");
        return;
    }
    resetStateAndList(); // Clears _displayList and resets _currentState, variables
    mp_wdt_reset();
    clearInterrupt();

    int commandCounter = 0;
    for (const auto& cmd : *_commands) {
        processCommandForDisplayList(cmd, -1); // loopIndex = -1 for top-level
        commandCounter++;
        if (commandCounter > 0 && commandCounter % 50 == 0) { // Yield less frequently
            yield();
            if (commandCounter % 150 == 0) {
                 mp_wdt_reset();
            }
        }
        if (_interrupt_requested) break;
    }
    mp_wdt_reset();
}

const std::vector<DisplayListItem>& MicroPatternsRuntime::getDisplayList() const {
    return _displayList;
}

void MicroPatternsRuntime::processCommandForDisplayList(const MicroPatternsCommand& cmd, int loopIndex) {
    if (_interrupt_requested || (_interrupt_check_cb && _interrupt_check_cb())) {
        _interrupt_requested = true;
        return;
    }

    // State-changing and control-flow commands are handled first and return
    // early. Only the drawing cases below build a DisplayListItem -- the old
    // code constructed (and snapshotted 52 bytes of transform into) an item for
    // every command including VAR/LET/IF/REPEAT, then threw most of them away.
    switch (cmd.type) {
        case CMD_VAR: {
            int slot = slotForCommandTarget(cmd, cmd.varName);
            int userSlot = slot - ENV_SLOT_COUNT;
            int value = cmd.initialExpressionTokens.empty() ? 0 : evaluateExpression(cmd.initialExpressionTokens, cmd.lineNumber, loopIndex);
            // Bounds-checked write -- see the note in resolveValue(). userSlot < 0
            // means the name resolved to an env slot, which VAR must not assign.
            if (userSlot >= 0 && (size_t)userSlot < _varValues.size()) {
                _varValues[userSlot] = value;
                _varDefined[userSlot] = 1;
            }
            return; // VAR does not generate a display list item
        }
        case CMD_LET: {
            int slot = slotForCommandTarget(cmd, cmd.letTargetVar);
            int userSlot = slot - ENV_SLOT_COUNT;
            if (userSlot >= 0 && (size_t)userSlot < _varValues.size() && _varDefined[userSlot]) {
                _varValues[userSlot] = cmd.letExpressionTokens.empty() ? 0 : evaluateExpression(cmd.letExpressionTokens, cmd.lineNumber, loopIndex);
            } else {
                runtimeError("LET: Undeclared variable: $" + cmd.letTargetVar, cmd.lineNumber);
            }
            return; // LET does not generate a display list item
        }
        case CMD_COLOR: {
            // Compared in place: no String copy, no toUpperCase allocation.
            const ParamValue* nameVal = findParam(cmd.params, "NAME");
            uint8_t resolvedColor = RUNTIME_COLOR_BLACK; // default, and the fallback on error
            if (nameVal) {
                if (nameVal->type == ParamValue::TYPE_STRING) {
                    // NULL-guarded: see findParam(). COLOR NAME="" reaches here
                    // with an empty String, whose c_str() is NULL on device.
                    const char* n = nameVal->stringValue.c_str();
                    if (n && strcasecmp(n, "WHITE") == 0) resolvedColor = RUNTIME_COLOR_WHITE;
                } else {
                    runtimeError("Parameter NAME requires a string/keyword.", cmd.lineNumber);
                }
            }
            _currentState.color = resolvedColor;
            return; // State change, no display list item
        }
        case CMD_FILL: {
            const MicroPatternsAsset* asset = nullptr;
            uint8_t kind = resolveAssetForCommand(cmd, asset);
            _currentState.fillAsset = (kind == 2) ? asset : nullptr;
            if (kind == 3) {
                const ParamValue* nameVal = findParam(cmd.params, "NAME");
                runtimeError("Undefined fill pattern: " + (nameVal ? nameVal->stringValue : String("?")), cmd.lineNumber);
            }
            return; // State change
        }
        case CMD_RESET_TRANSFORMS:
            _currentState.scale = 1.0f;
            matrix_identity(_currentState.matrix);
            matrix_identity(_currentState.inverseMatrix);
            _xfDirty = true;
            return; // State change
        case CMD_TRANSLATE: {
            float dx = static_cast<float>(resolveIntParam("DX", cmd.params, 0, cmd.lineNumber, loopIndex));
            float dy = static_cast<float>(resolveIntParam("DY", cmd.params, 0, cmd.lineNumber, loopIndex));
            float T_op[6]; matrix_make_translation(T_op, dx, dy);
            matrix_multiply(_currentState.matrix, _currentState.matrix, T_op);
            if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
            _xfDirty = true;
            return; // State change
        }
        case CMD_ROTATE: {
            float degrees = static_cast<float>(resolveIntParam("DEGREES", cmd.params, 0, cmd.lineNumber, loopIndex));
            float R_op[6]; matrix_make_rotation(R_op, degrees);
            matrix_multiply(_currentState.matrix, _currentState.matrix, R_op);
            if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
            _xfDirty = true;
            return; // State change
        }
        case CMD_SCALE:
            _currentState.scale = std::max(1, resolveIntParam("FACTOR", cmd.params, 1, cmd.lineNumber, loopIndex));
            _xfDirty = true;
            return; // State change

        // Control Flow
        case CMD_REPEAT: {
            int count = resolveValue(cmd.count, cmd.lineNumber, loopIndex);
            if (count < 0) { runtimeError("REPEAT count negative.", cmd.lineNumber); return; }
            mp_wdt_reset();
            for (int i = 0; i < count; ++i) {
                for (const auto& nestedCmd : cmd.nestedCommands) {
                    processCommandForDisplayList(nestedCmd, i);
                    if (_interrupt_requested) break;
                }
                if (_interrupt_requested) break;
                if (i > 0 && i % 20 == 0) { yield(); if (i % 60 == 0) mp_wdt_reset(); }
            }
            return; // REPEAT block expanded, no single item for REPEAT itself
        }
        case CMD_IF: {
            bool conditionMet = evaluateCondition(cmd.conditionTokens, cmd.lineNumber, loopIndex);
            const auto& commandsToRun = conditionMet ? cmd.thenCommands : cmd.elseCommands;
            for (const auto& nestedCmd : commandsToRun) {
                processCommandForDisplayList(nestedCmd, loopIndex); // Pass outer loopIndex
                if (_interrupt_requested) break;
            }
            return; // IF block expanded
        }

        case CMD_PIXEL:
        case CMD_FILL_PIXEL:
        case CMD_LINE:
        case CMD_RECT:
        case CMD_FILL_RECT:
        case CMD_CIRCLE:
        case CMD_FILL_CIRCLE:
        case CMD_DRAW:
            break; // fall through to item construction below

        default: // CMD_UNKNOWN, CMD_DEFINE_PATTERN, CMD_NOOP, CMD_ENDREPEAT, CMD_ELSE, CMD_ENDIF
            return; // Do not add to display list
    }

    DisplayListItem dlItem;
    dlItem.type = cmd.type;
    dlItem.sourceLine = cmd.lineNumber;
    dlItem.xf = currentTransform();
    dlItem.color = _currentState.color;
    dlItem.fillAsset = _currentState.fillAsset;

    switch (cmd.type) {
        case CMD_PIXEL:
        case CMD_FILL_PIXEL:
            dlItem.p[0] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[1] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.isOpaque = true;
            break;
        case CMD_LINE:
            dlItem.p[0] = resolveIntParam("X1", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[1] = resolveIntParam("Y1", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[2] = resolveIntParam("X2", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[3] = resolveIntParam("Y2", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_RECT:
        case CMD_FILL_RECT:
            dlItem.p[0] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[1] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[2] = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[3] = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.isOpaque = (cmd.type == CMD_FILL_RECT);
            break;
        case CMD_CIRCLE:
        case CMD_FILL_CIRCLE:
            dlItem.p[0] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[1] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[2] = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.isOpaque = (cmd.type == CMD_FILL_CIRCLE);
            break;
        case CMD_DRAW: {
            dlItem.p[0] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.p[1] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            // Resolve the asset once, here, instead of storing its name and
            // making the rasterizer look it up in a String-keyed map per item.
            const MicroPatternsAsset* asset = nullptr;
            if (resolveAssetForCommand(cmd, asset) != 2) {
                const ParamValue* nameVal = findParam(cmd.params, "NAME");
                runtimeError("DRAW: Invalid asset name '" + (nameVal ? nameVal->stringValue : String("SOLID")) + "'.", cmd.lineNumber);
                return; // Don't add invalid DRAW to list
            }
            dlItem.asset = asset;
            dlItem.isOpaque = isAssetDataFullyOpaque(dlItem.asset);
            break;
        }
        default:
            return;
    }

    _displayList.push_back(dlItem);
}
