#include "micropatterns_runtime.h"
#include "esp32-hal-log.h"
#include <Arduino.h>
#include "matrix_utils.h"
// #include <GxEPD2_BW.h> // No longer needed directly, use HAL colors
#include "IDrawingHAL.h" // For HAL_COLOR_BLACK, HAL_COLOR_WHITE

// RUNTIME_COLOR constants are removed, directly use HAL_COLOR_BLACK/WHITE

MicroPatternsRuntime::MicroPatternsRuntime(IDrawingHAL* hal, const std::map<String, MicroPatternsAsset>& assets)
    : _hal(hal), _assets(assets), _interrupt_requested(false), _interrupt_check_cb(nullptr) {
    // Initialize canvas dimensions from HAL
    _canvasWidth = _hal ? _hal->getScreenWidth() : 200; // Default to 200 if HAL is null
    _canvasHeight = _hal ? _hal->getScreenHeight() : 200; // Default to 200 if HAL is null
    
    resetStateAndList(); // This will set _currentState.color to HAL_COLOR_BLACK by default
    _environment["$WIDTH"] = _canvasWidth;
    _environment["$HEIGHT"] = _canvasHeight;
    _environment["$HOUR"] = 0;
    _environment["$MINUTE"] = 0;
    _environment["$SECOND"] = 0;
    _environment["$COUNTER"] = 0;
}

int MicroPatternsRuntime::getCounter() const {
    return _environment.count("$COUNTER") ? _environment.at("$COUNTER") : 0;
}

void MicroPatternsRuntime::getTime(int& hour, int& minute, int& second) const {
    hour = _environment.count("$HOUR") ? _environment.at("$HOUR") : 0;
    minute = _environment.count("$MINUTE") ? _environment.at("$MINUTE") : 0;
    second = _environment.count("$SECOND") ? _environment.at("$SECOND") : 0;
}

void MicroPatternsRuntime::setCommands(const std::list<MicroPatternsCommand>* commands) {
    _commands = commands;
}

void MicroPatternsRuntime::setDeclaredVariables(const std::set<String>* declaredVariables) {
    _declaredVariables = declaredVariables;
}

void MicroPatternsRuntime::resetStateAndList() {
    _currentState = MicroPatternsState();
    _variables.clear();
    _environment.erase("$INDEX");
    _displayList.clear();
    // _displayList.reserve(200); // Pre-allocate some space if average list size is known
}

void MicroPatternsRuntime::setCounter(int counter) {
    _environment["$COUNTER"] = counter;
}

void MicroPatternsRuntime::setTime(int hour, int minute, int second) {
    _environment["$HOUR"] = hour;
    _environment["$MINUTE"] = minute;
    _environment["$SECOND"] = second;
}

void MicroPatternsRuntime::runtimeError(const String& message, int lineNumber) {
    log_e("Runtime Error (Line %d): %s", lineNumber, message.c_str());
}

int MicroPatternsRuntime::resolveValue(const ParamValue& val, int lineNumber, int loopIndex) {
    if (val.type == ParamValue::TYPE_INT) {
        return val.intValue;
    } else if (val.type == ParamValue::TYPE_VARIABLE) {
        String varName = val.stringValue;
        varName.toUpperCase();
        if (varName == "$INDEX") {
            if (loopIndex < 0) {
                runtimeError("Variable $INDEX can only be used inside a REPEAT loop.", lineNumber);
                return 0;
            }
            return loopIndex;
        }
        if (_environment.count(varName)) return _environment.at(varName);
        if (_variables.count(varName)) return _variables.at(varName);
        runtimeError("Undefined variable: " + val.stringValue, lineNumber);
        return 0;
    }
    runtimeError("Expected integer or variable, got: " + val.stringValue, lineNumber);
    return 0;
}

int MicroPatternsRuntime::resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber, int loopIndex) {
    String upperParamName = paramName;
    upperParamName.toUpperCase();
    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_INT || val.type == ParamValue::TYPE_VARIABLE) {
            return resolveValue(val, lineNumber, loopIndex);
        }
        runtimeError("Parameter " + paramName + " requires an integer or variable.", lineNumber);
    }
    return defaultValue;
}

String MicroPatternsRuntime::resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber) {
    String upperParamName = paramName;
    upperParamName.toUpperCase();
    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_STRING) {
            return val.stringValue;
        }
        runtimeError("Parameter " + paramName + " requires a string/keyword.", lineNumber);
    }
    return defaultValue;
}

String MicroPatternsRuntime::resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params, int lineNumber) {
    String upperParamName = paramName;
    upperParamName.toUpperCase();
    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_STRING) {
            String nameValue = val.stringValue;
            nameValue.toUpperCase();
            return nameValue; // SOLID or UPPERCASE pattern name
        }
        runtimeError("Parameter " + paramName + " requires SOLID or a pattern name string.", lineNumber);
    }
    return "SOLID"; // Default
}

int MicroPatternsRuntime::evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex) {
    if (tokens.empty()) return 0;

    std::vector<ParamValue> resolvedTokens;
    for (const auto& token : tokens) {
        if (token.type == ParamValue::TYPE_VARIABLE) {
            resolvedTokens.push_back(ParamValue(resolveValue(token, lineNumber, loopIndex)));
        } else if (token.type == ParamValue::TYPE_INT || token.type == ParamValue::TYPE_OPERATOR) {
            resolvedTokens.push_back(token);
        } else {
            runtimeError("Unexpected token type in expression: " + token.stringValue, lineNumber);
            return 0;
        }
    }
    
    // Basic structural checks (simplified for brevity, more robust checks in JS version)
    if (resolvedTokens.empty() && !tokens.empty()) { /* Error if original tokens not empty but resolved is */ return 0; }
    if (resolvedTokens.empty() && tokens.empty()) { return 0; } // Empty expression is 0
    if (resolvedTokens.back().type == ParamValue::TYPE_OPERATOR) { /* Error */ return 0; }
    if (resolvedTokens.front().type == ParamValue::TYPE_OPERATOR) { /* Error */ return 0; }


    std::vector<ParamValue> pass1Result; // For MDM operators (*, /, %)
    for (size_t i = 0; i < resolvedTokens.size(); ++i) {
        const ParamValue& currentToken = resolvedTokens[i];
        if (currentToken.type == ParamValue::TYPE_OPERATOR && (currentToken.stringValue == "*" || currentToken.stringValue == "/" || currentToken.stringValue == "%")) {
            if (pass1Result.empty() || pass1Result.back().type != ParamValue::TYPE_INT || i + 1 >= resolvedTokens.size() || resolvedTokens[i+1].type != ParamValue::TYPE_INT) {
                runtimeError("Syntax error with operator " + currentToken.stringValue, lineNumber); return 0;
            }
            int leftVal = pass1Result.back().intValue; pass1Result.pop_back();
            int rightVal = resolvedTokens[i+1].intValue;
            int result = 0;
            if (currentToken.stringValue == "*") result = leftVal * rightVal;
            else if (currentToken.stringValue == "/") { if (rightVal == 0) { runtimeError("Division by zero.", lineNumber); return 0; } result = leftVal / rightVal; }
            else if (currentToken.stringValue == "%") { if (rightVal == 0) { runtimeError("Modulo by zero.", lineNumber); return 0; } result = leftVal % rightVal; }
            pass1Result.push_back(ParamValue(result));
            i++; // Skip right operand
        } else {
            pass1Result.push_back(currentToken);
        }
    }

    if (pass1Result.empty()) return 0; // Should not happen if resolvedTokens was not empty
    int finalResult = pass1Result[0].intValue; // Must start with a number
    for (size_t i = 1; i < pass1Result.size(); i += 2) {
        if (i + 1 >= pass1Result.size() || pass1Result[i].type != ParamValue::TYPE_OPERATOR || pass1Result[i+1].type != ParamValue::TYPE_INT) {
            runtimeError("Syntax error in expression (AS pass).", lineNumber); return 0;
        }
        String op = pass1Result[i].stringValue;
        int rightVal = pass1Result[i+1].intValue;
        if (op == "+") finalResult += rightVal;
        else if (op == "-") finalResult -= rightVal;
        else { runtimeError("Unexpected operator in AS pass: " + op, lineNumber); return 0; }
    }
    return finalResult;
}

bool MicroPatternsRuntime::evaluateCondition(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex) {
    if (tokens.empty()) { runtimeError("Empty condition.", lineNumber); return false; }
    
    int comparisonOpIndex = -1; String comparisonOp = "";
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == ParamValue::TYPE_OPERATOR) {
            const String& opStr = tokens[i].stringValue;
            if (opStr == "==" || opStr == "!=" || opStr == "<" || opStr == ">" || opStr == "<=" || opStr == ">=") {
                if (comparisonOpIndex != -1) { runtimeError("Multiple comparison operators.", lineNumber); return false; }
                comparisonOpIndex = i; comparisonOp = opStr;
            }
        }
    }
    if (comparisonOpIndex == -1) { runtimeError("No comparison operator in condition.", lineNumber); return false; }

    std::vector<ParamValue> leftTokens(tokens.begin(), tokens.begin() + comparisonOpIndex);
    std::vector<ParamValue> rightTokens(tokens.begin() + comparisonOpIndex + 1, tokens.end());
    if (leftTokens.empty() || rightTokens.empty()) { runtimeError("Missing operand in condition.", lineNumber); return false; }

    int leftValue = evaluateExpression(leftTokens, lineNumber, loopIndex);
    int rightValue = evaluateExpression(rightTokens, lineNumber, loopIndex);

    if (comparisonOp == "==") return leftValue == rightValue;
    if (comparisonOp == "!=") return leftValue != rightValue;
    if (comparisonOp == "<")  return leftValue < rightValue;
    if (comparisonOp == ">")  return leftValue > rightValue;
    if (comparisonOp == "<=") return leftValue <= rightValue;
    if (comparisonOp == ">=") return leftValue >= rightValue;
    runtimeError("Unknown comparison operator: " + comparisonOp, lineNumber);
    return false;
}

bool MicroPatternsRuntime::isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const {
    if (!asset || asset->data.empty()) return false;
    for (uint8_t pixelValue : asset->data) {
        if (pixelValue == 0) return false; // 0 is transparent for DRAW
    }
    return true;
}

bool MicroPatternsRuntime::determineItemOpacity(CommandType type, const std::map<String, ParamValue>& params, const String& assetNameParamValue) const {
    if (type == CMD_FILL_RECT || type == CMD_FILL_CIRCLE || type == CMD_FILL_PIXEL || type == CMD_PIXEL) {
        return true;
    }
    if (type == CMD_DRAW) {
        // assetNameParamValue is already resolved, uppercase name or "SOLID"
        if (assetNameParamValue != "SOLID" && _assets.count(assetNameParamValue)) {
            return isAssetDataFullyOpaque(&_assets.at(assetNameParamValue));
        }
        return false;
    }
    return false;
}


void MicroPatternsRuntime::generateDisplayList() {
    if (!_commands || !_declaredVariables) {
        log_e("Runtime not properly initialized for display list generation.");
        return;
    }
    resetStateAndList(); // Clears _displayList and resets _currentState, _variables
    esp_task_wdt_reset();
    clearInterrupt();

    int commandCounter = 0;
    for (const auto& cmd : *_commands) {
        processCommandForDisplayList(cmd, -1); // loopIndex = -1 for top-level
        commandCounter++;
        if (commandCounter > 0 && commandCounter % 50 == 0) { // Yield less frequently
            yield();
            if (commandCounter % 150 == 0) {
                 esp_task_wdt_reset();
            }
        }
        if (_interrupt_requested) break;
    }
    esp_task_wdt_reset();
}

const std::vector<DisplayListItem>& MicroPatternsRuntime::getDisplayList() const {
    return _displayList;
}

void MicroPatternsRuntime::processCommandForDisplayList(const MicroPatternsCommand& cmd, int loopIndex) {
    if (_interrupt_requested || (_interrupt_check_cb && _interrupt_check_cb())) {
        _interrupt_requested = true;
        return;
    }

    DisplayListItem dlItem;
    dlItem.type = cmd.type;
    dlItem.sourceLine = cmd.lineNumber;

    // Snapshot current state common to most drawing items
    memcpy(dlItem.matrix, _currentState.matrix, sizeof(float) * 6);
    memcpy(dlItem.inverseMatrix, _currentState.inverseMatrix, sizeof(float) * 6);
    dlItem.scaleFactor = _currentState.scale;
    dlItem.color = _currentState.color;
    dlItem.fillAsset = _currentState.fillAsset;
    
    // Temp storage for resolved asset name for opacity check
    String resolvedAssetNameForOpacity = "";


    switch (cmd.type) {
        case CMD_VAR: {
            String varKey = "$" + cmd.varName;
            _variables[varKey] = cmd.initialExpressionTokens.empty() ? 0 : evaluateExpression(cmd.initialExpressionTokens, cmd.lineNumber, loopIndex);
            return; // VAR does not generate a display list item
        }
        case CMD_LET: {
            String targetVarKey = "$" + cmd.letTargetVar;
            if (_variables.count(targetVarKey)) {
                _variables[targetVarKey] = cmd.letExpressionTokens.empty() ? 0 : evaluateExpression(cmd.letExpressionTokens, cmd.lineNumber, loopIndex);
            } else {
                runtimeError("LET: Undeclared variable: " + targetVarKey, cmd.lineNumber);
            }
            return; // LET does not generate a display list item
        }
        case CMD_COLOR: {
            String colorName = resolveStringParam("NAME", cmd.params, "BLACK", cmd.lineNumber);
            colorName.toUpperCase();
            // _currentState.color is uint16_t, set to HAL_COLOR_BLACK or HAL_COLOR_WHITE
            if (colorName == "WHITE") {
                _currentState.color = HAL_COLOR_WHITE;
            } else if (colorName == "BLACK") {
                _currentState.color = HAL_COLOR_BLACK;
            } else {
                runtimeError("Invalid color name: " + colorName, cmd.lineNumber);
                // Default to black if invalid color name
                _currentState.color = HAL_COLOR_BLACK;
            }
            return; // State change, no display list item
        }
        case CMD_FILL: {
            String fillName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber); // SOLID or UPPERCASE pattern
            _currentState.fillAsset = (fillName == "SOLID" || !_assets.count(fillName)) ? nullptr : &_assets.at(fillName);
            if (fillName != "SOLID" && !_assets.count(fillName)) {
                runtimeError("Undefined fill pattern: " + fillName, cmd.lineNumber);
            }
            return; // State change
        }
        case CMD_RESET_TRANSFORMS:
            _currentState.scale = 1.0f;
            matrix_identity(_currentState.matrix);
            matrix_identity(_currentState.inverseMatrix);
            return; // State change
        case CMD_TRANSLATE: {
            float dx = static_cast<float>(resolveIntParam("DX", cmd.params, 0, cmd.lineNumber, loopIndex));
            float dy = static_cast<float>(resolveIntParam("DY", cmd.params, 0, cmd.lineNumber, loopIndex));
            float T_op[6]; matrix_make_translation(T_op, dx, dy);
            matrix_multiply(_currentState.matrix, _currentState.matrix, T_op);
            if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
            return; // State change
        }
        case CMD_ROTATE: {
            float degrees = static_cast<float>(resolveIntParam("DEGREES", cmd.params, 0, cmd.lineNumber, loopIndex));
            float R_op[6]; matrix_make_rotation(R_op, degrees);
            matrix_multiply(_currentState.matrix, _currentState.matrix, R_op);
            if (!matrix_invert(_currentState.inverseMatrix, _currentState.matrix)) { /* error */ }
            return; // State change
        }
        case CMD_SCALE:
            _currentState.scale = std::max(1, resolveIntParam("FACTOR", cmd.params, 1, cmd.lineNumber, loopIndex));
            return; // State change

        // Drawing commands - resolve params and add to list
        case CMD_PIXEL:
            dlItem.intParams["X"] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y"] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_FILL_PIXEL:
            dlItem.intParams["X"] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y"] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_LINE:
            dlItem.intParams["X1"] = resolveIntParam("X1", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y1"] = resolveIntParam("Y1", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["X2"] = resolveIntParam("X2", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y2"] = resolveIntParam("Y2", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_RECT:
        case CMD_FILL_RECT:
            dlItem.intParams["X"] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y"] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["WIDTH"] = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["HEIGHT"] = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_CIRCLE:
        case CMD_FILL_CIRCLE:
            dlItem.intParams["X"] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y"] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["RADIUS"] = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber, loopIndex);
            break;
        case CMD_DRAW:
            dlItem.intParams["X"] = resolveIntParam("X", cmd.params, 0, cmd.lineNumber, loopIndex);
            dlItem.intParams["Y"] = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber, loopIndex);
            resolvedAssetNameForOpacity = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber);
            dlItem.stringParams["NAME"] = resolvedAssetNameForOpacity;
            if (resolvedAssetNameForOpacity == "SOLID" || !_assets.count(resolvedAssetNameForOpacity)) {
                runtimeError("DRAW: Invalid asset name '" + resolvedAssetNameForOpacity + "'.", cmd.lineNumber);
                return; // Don't add invalid DRAW to list
            }
            break;
        
        // Control Flow
        case CMD_REPEAT: {
            int count = resolveValue(cmd.count, cmd.lineNumber, loopIndex);
            if (count < 0) { runtimeError("REPEAT count negative.", cmd.lineNumber); return; }
            esp_task_wdt_reset();
            int previousIndex = _environment.count("$INDEX") ? _environment.at("$INDEX") : -1;
            for (int i = 0; i < count; ++i) {
                _environment["$INDEX"] = i;
                for (const auto& nestedCmd : cmd.nestedCommands) {
                    processCommandForDisplayList(nestedCmd, i);
                    if (_interrupt_requested) break;
                }
                if (_interrupt_requested) break;
                if (i > 0 && i % 20 == 0) { yield(); if (i % 60 == 0) esp_task_wdt_reset(); }
            }
            if (previousIndex != -1) _environment["$INDEX"] = previousIndex; else _environment.erase("$INDEX");
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
        default: // CMD_UNKNOWN, CMD_DEFINE_PATTERN, CMD_NOOP, CMD_ENDREPEAT, CMD_ELSE, CMD_ENDIF
            return; // Do not add to display list
    }

    // Determine opacity for the item being added
    dlItem.isOpaque = determineItemOpacity(dlItem.type, cmd.params, resolvedAssetNameForOpacity);
    _displayList.push_back(dlItem);
}
