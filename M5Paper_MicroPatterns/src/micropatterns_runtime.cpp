#include "micropatterns_runtime.h"
#include "esp32-hal-log.h" // For logging errors

// Define colors (consistent with drawing class)
const uint8_t RUNTIME_COLOR_WHITE = 0;
const uint8_t RUNTIME_COLOR_BLACK = 15;

MicroPatternsRuntime::MicroPatternsRuntime(M5EPD_Canvas* canvas, const std::map<String, MicroPatternsAsset>& assets)
    : _canvas(canvas), _drawing(canvas), _assets(assets) {
    resetState();
    // Initialize environment variables
    _environment["$WIDTH"] = _canvas ? _canvas->width() : 540; // Default or actual
    _environment["$HEIGHT"] = _canvas ? _canvas->height() : 960; // Default or actual
    _environment["$COUNTER"] = 0;
    // $HOUR, $MINUTE, $SECOND, $INDEX would be set externally if needed
}

void MicroPatternsRuntime::setCommands(const std::vector<MicroPatternsCommand>* commands) {
    _commands = commands;
}

void MicroPatternsRuntime::setDeclaredVariables(const std::vector<String>* declaredVariables) {
    _declaredVariables = declaredVariables;
}


void MicroPatternsRuntime::resetState() {
    _currentState = MicroPatternsState(); // Reset to default state
    _variables.clear(); // Clear user variables
    // Re-initialize environment variables that might change
    _environment["$COUNTER"] = 0;
    // NOTE: Declared variables are now initialized to 0 when the VAR command is executed.
}

void MicroPatternsRuntime::setCounter(int counter) {
    _environment["$COUNTER"] = counter;
}

void MicroPatternsRuntime::runtimeError(const String& message, int lineNumber) {
    // Simple error logging for now
    log_e("Runtime Error (Line %d): %s", lineNumber, message.c_str());
    // Could potentially halt execution or store errors
}

// --- Parameter Resolution ---

int MicroPatternsRuntime::resolveValue(const ParamValue& val) {
    if (val.type == ParamValue::TYPE_INT) {
        return val.intValue;
    } else if (val.type == ParamValue::TYPE_VARIABLE) {
        // Variable names in ParamValue are stored UPPERCASE including '$'
        String varNameUpper = val.stringValue; // e.g., "$MYVAR" or "$COUNTER"

        // Check environment first
        if (_environment.count(varNameUpper)) {
            return _environment[varNameUpper];
        }
        // Check user variables (stored without '$', UPPERCASE)
        String userVarName = varNameUpper.substring(1); // Remove '$'
        if (_variables.count(userVarName)) {
            return _variables[userVarName];
        } else {
            // This should ideally be caught by parser, but check defensively
            runtimeError("Undefined variable: " + val.stringValue, 0); // Line number unknown here
            return 0; // Default value on error
        }
    } else {
        // String literal - cannot resolve to int directly
        runtimeError("Expected integer or variable, got string literal: " + val.stringValue, 0);
        return 0; // Default value on error
    }
}


int MicroPatternsRuntime::resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue) {
    String upperParamName = paramName;
    upperParamName.toUpperCase(); // Ensure lookup is case-insensitive

    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_INT) {
            return val.intValue;
        } else if (val.type == ParamValue::TYPE_VARIABLE) {
            return resolveValue(val); // Resolve the variable
        } else {
            runtimeError("Parameter " + paramName + " requires an integer or variable, got string.", 0);
            return defaultValue;
        }
    }
    runtimeError("Missing required parameter: " + paramName, 0);
    return defaultValue;
}

String MicroPatternsRuntime::resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue) {
     String upperParamName = paramName;
     upperParamName.toUpperCase();

    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_STRING) {
            return val.stringValue;
        } else {
            runtimeError("Parameter " + paramName + " requires a string literal.", 0);
            return defaultValue;
        }
    }
     runtimeError("Missing required parameter: " + paramName, 0);
    return defaultValue;
}

// Handles NAME=SOLID or NAME="pattern"
String MicroPatternsRuntime::resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params) {
    String upperParamName = paramName;
    upperParamName.toUpperCase();

    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_STRING) {
            String nameValue = val.stringValue;
            nameValue.toUpperCase(); // Compare case-insensitively
            if (nameValue == "SOLID") {
                return "SOLID"; // Return normalized keyword
            } else {
                // Return the pattern name (already uppercase from parser if variable, or from string here)
                return nameValue;
            }
        } else {
             runtimeError("Parameter " + paramName + " requires SOLID or a pattern name string.", 0);
            return "SOLID"; // Default to solid on error
        }
    }
     runtimeError("Missing required parameter: " + paramName, 0);
    return "SOLID"; // Default to solid on error
}


// --- Execution ---

void MicroPatternsRuntime::execute() {
    if (!_commands || !_canvas) {
        log_e("Runtime not properly initialized (commands or canvas missing).");
        return;
    }

    resetState(); // Reset state before each full execution
    _drawing.clearCanvas(); // Start with a clear (white) canvas

    for (const auto& cmd : *_commands) {
        executeCommand(cmd);
    }

    // After executing all commands, push the final canvas to the display
    // Use the specified GLD16 mode for 1-bit rendering
    _canvas->pushCanvas(0, 0, UPDATE_MODE_GLD16);
}

void MicroPatternsRuntime::executeCommand(const MicroPatternsCommand& cmd) {
    // Resolve parameters common to many drawing commands
    // Using default 0 is okay for coordinates/sizes as it likely draws nothing or a dot.
    // Using default 1 for scale factor.
    int x = resolveIntParam("X", cmd.params, 0);
    int y = resolveIntParam("Y", cmd.params, 0);
    int x1 = resolveIntParam("X1", cmd.params, 0);
    int y1 = resolveIntParam("Y1", cmd.params, 0);
    int x2 = resolveIntParam("X2", cmd.params, 0);
    int y2 = resolveIntParam("Y2", cmd.params, 0);
    int w = resolveIntParam("WIDTH", cmd.params, 0);
    int h = resolveIntParam("HEIGHT", cmd.params, 0);
    int r = resolveIntParam("RADIUS", cmd.params, 0);
    int dx = resolveIntParam("DX", cmd.params, 0);
    int dy = resolveIntParam("DY", cmd.params, 0);
    int degrees = resolveIntParam("DEGREES", cmd.params, 0);
    int factor = resolveIntParam("FACTOR", cmd.params, 1); // Default scale = 1

    switch (cmd.type) {
        case CMD_VAR:
            // Initialize the declared variable to 0 in the map.
            // cmd.varName is UPPERCASE, no '$'
            if (!_variables.count(cmd.varName)) { // Check if already initialized (e.g., by a previous VAR for the same name - parser should prevent this)
                 _variables[cmd.varName] = 0;
            } else {
                 // This case should ideally not happen if parser prevents re-declaration
                 runtimeError("Variable $" + cmd.varName + " already initialized.", cmd.lineNumber);
            }
            // TODO: Add handling for initialExpression if implemented in parser
            break;

        case CMD_LET:
             // Assignment: Resolve the value and assign to target var
             // cmd.letTargetVar is UPPERCASE, no '$'
             if (_variables.count(cmd.letTargetVar)) {
                 // Resolve the value from the parsed parameter
                 int valueToAssign = resolveIntParam("VALUE", cmd.params, 0); // Using resolveIntParam handles int literal or variable
                 _variables[cmd.letTargetVar] = valueToAssign;
             } else {
                 // This should be caught by parser, but check defensively
                 runtimeError("Attempted to assign to undeclared variable: $" + cmd.letTargetVar, cmd.lineNumber);
             }
            break;

        case CMD_COLOR: {
            String colorName = resolveStringParam("NAME", cmd.params, "BLACK");
            colorName.toUpperCase();
            if (colorName == "WHITE") {
                _currentState.color = RUNTIME_COLOR_WHITE;
            } else if (colorName == "BLACK") {
                _currentState.color = RUNTIME_COLOR_BLACK;
            } else {
                runtimeError("Invalid COLOR NAME: " + colorName, cmd.lineNumber);
            }
            break;
        }
        case CMD_FILL: {
            String fillName = resolveAssetNameParam("NAME", cmd.params); // Handles SOLID or pattern name
            if (fillName == "SOLID") {
                _currentState.fillAsset = nullptr;
            } else {
                // fillName is already uppercase
                if (_assets.count(fillName)) {
                    _currentState.fillAsset = &_assets.at(fillName);
                } else {
                    runtimeError("Undefined fill pattern: " + fillName, cmd.lineNumber);
                    _currentState.fillAsset = nullptr; // Default to solid on error
                }
            }
            break;
        }
        case CMD_RESET_TRANSFORMS:
            _currentState.translateX = 0;
            _currentState.translateY = 0;
            _currentState.rotationDegrees = 0;
            _currentState.scale = 1.0;
            break;
        case CMD_TRANSLATE:
            // Simplified: Absolute translation, not cumulative stack
            _currentState.translateX = dx;
            _currentState.translateY = dy;
            break;
        case CMD_ROTATE:
             // Simplified: Absolute rotation
            _currentState.rotationDegrees = degrees % 360;
             if (_currentState.rotationDegrees < 0) _currentState.rotationDegrees += 360;
            break;
        case CMD_SCALE:
             // Simplified: Absolute scale
            _currentState.scale = (factor >= 1) ? factor : 1.0;
            break;

        // Drawing commands
        case CMD_PIXEL:
            _drawing.drawPixel(x, y, _currentState);
            break;
        case CMD_FILL_PIXEL:
             _drawing.drawFilledPixel(x, y, _currentState);
             break;
        case CMD_LINE:
            _drawing.drawLine(x1, y1, x2, y2, _currentState);
            break;
        case CMD_RECT:
            _drawing.drawRect(x, y, w, h, _currentState);
            break;
        case CMD_FILL_RECT:
            _drawing.fillRect(x, y, w, h, _currentState);
            break;
        case CMD_CIRCLE:
            _drawing.drawCircle(x, y, r, _currentState);
            break;
        case CMD_FILL_CIRCLE:
            _drawing.fillCircle(x, y, r, _currentState);
            break;
        case CMD_DRAW: {
            String assetName = resolveAssetNameParam("NAME", cmd.params);
             if (assetName != "SOLID") { // Can't DRAW solid
                 if (_assets.count(assetName)) {
                     _drawing.drawAsset(x, y, _assets.at(assetName), _currentState);
                 } else {
                     runtimeError("Undefined asset for DRAW: " + assetName, cmd.lineNumber);
                 }
             } else {
                  runtimeError("Cannot DRAW SOLID.", cmd.lineNumber);
             }
            break;
        }

        case CMD_UNKNOWN:
        case CMD_DEFINE_PATTERN: // Handled by parser
        case CMD_NOOP:
            // Do nothing
            break;

        // Not implemented in this basic version
        case CMD_REPEAT:
        case CMD_IF:
             runtimeError("Command not implemented: " + String(cmd.type), cmd.lineNumber);
            break;
    }
}