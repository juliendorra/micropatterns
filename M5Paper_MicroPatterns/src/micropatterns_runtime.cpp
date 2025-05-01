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

// Resolves a single value (int literal or variable reference)
// Uses lineNumber for error reporting.
int MicroPatternsRuntime::resolveValue(const ParamValue& val, int lineNumber) {
    if (val.type == ParamValue::TYPE_INT) {
        return val.intValue;
    } else if (val.type == ParamValue::TYPE_VARIABLE) {
        // Variable names in ParamValue are stored UPPERCASE including '$'
        String varNameUpper = val.stringValue; // e.g., "$MYVAR" or "$COUNTER"

        // Check environment first (keys are like "$COUNTER")
        if (_environment.count(varNameUpper)) {
            return _environment.at(varNameUpper);
        }
        // Check user variables (keys are like "MYVAR", without '$')
        String userVarName = varNameUpper.substring(1); // Remove '$'
        if (_variables.count(userVarName)) {
            return _variables.at(userVarName);
        } else {
            // This should ideally be caught by parser, but check defensively
            runtimeError("Undefined variable: " + val.stringValue, lineNumber);
            return 0; // Default value on error
        }
    } else {
        // String literal - cannot resolve to int directly
        runtimeError("Expected integer or variable, got string literal: " + val.stringValue, lineNumber);
        return 0; // Default value on error
    }
}


int MicroPatternsRuntime::resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber) {
    String upperParamName = paramName;
    upperParamName.toUpperCase(); // Ensure lookup is case-insensitive

    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_INT) {
            return val.intValue;
        } else if (val.type == ParamValue::TYPE_VARIABLE) {
            return resolveValue(val, lineNumber); // Resolve the variable, passing line number
        } else {
            runtimeError("Parameter " + paramName + " requires an integer or variable, got string.", lineNumber);
            return defaultValue;
        }
    }
    // Parameter not found - return default. This is not an error.
    return defaultValue;
}

String MicroPatternsRuntime::resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber) {
     String upperParamName = paramName;
     upperParamName.toUpperCase();

    if (params.count(upperParamName)) {
        const ParamValue& val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_STRING) {
            return val.stringValue;
        } else {
            runtimeError("Parameter " + paramName + " requires a string literal.", lineNumber);
            return defaultValue;
        }
    }
     // Return default value for missing parameters (optional parameters)
    return defaultValue;
}

// Handles NAME=SOLID or NAME="pattern"
String MicroPatternsRuntime::resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params, int lineNumber) {
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
                // Parser ensures it's a string literal, not a variable here.
                return nameValue;
            }
        } else {
             runtimeError("Parameter " + paramName + " requires SOLID or a pattern name string.", lineNumber);
            return "SOLID"; // Default to solid on error
        }
    }
     // Return default value for missing parameters (optional parameters)
    return "SOLID"; // Default to solid if NAME param is missing
}


// --- Expression Evaluation ---

// Helper to apply operation, returns result or logs error and returns 0
int applyOperation(int val1, const String& op, int val2, int lineNumber, MicroPatternsRuntime* runtime) {
    if (op == "+") return val1 + val2;
    if (op == "-") return val1 - val2;
    if (op == "*") return val1 * val2;
    if (op == "/") {
        if (val2 == 0) {
            runtime->runtimeError("Division by zero.", lineNumber);
            return 0;
        }
        return val1 / val2; // Integer division
    }
    if (op == "%") {
        if (val2 == 0) {
            runtime->runtimeError("Modulo by zero.", lineNumber);
            return 0;
        }
        return val1 % val2;
    }
    runtime->runtimeError("Unknown operator: " + op, lineNumber);
    return 0;
}

// Evaluates a sequence of tokens (numbers, variables, operators)
int MicroPatternsRuntime::evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber) {
    if (tokens.empty()) {
        // Parser should prevent this, but handle defensively.
        // An empty expression could result from "VAR $x ="
        // Return 0 for an empty expression, as VAR defaults to 0.
        // runtimeError("Cannot evaluate empty expression.", lineNumber); // Optional: Too noisy?
        return 0;
    }

    // 1. Resolve all variables to numbers
    std::vector<ParamValue> resolvedTokens;
    for (const auto& token : tokens) {
        if (token.type == ParamValue::TYPE_VARIABLE) {
            // Resolve variable and push as TYPE_INT
            resolvedTokens.push_back(ParamValue(resolveValue(token, lineNumber)));
        } else if (token.type == ParamValue::TYPE_INT || token.type == ParamValue::TYPE_STRING) {
             // Keep numbers and operators (operators are stored as TYPE_STRING)
            resolvedTokens.push_back(token);
        } else {
            runtimeError("Unexpected token type during expression resolution.", lineNumber);
            return 0;
        }
    }

    // Basic structural checks after resolution
    if (resolvedTokens.empty()) { // Should not happen if original tokens were not empty
        runtimeError("Expression resolved to empty token list.", lineNumber);
        return 0;
    }
    if (resolvedTokens[0].type == ParamValue::TYPE_STRING) {
        runtimeError("Expression cannot start with an operator: " + resolvedTokens[0].stringValue, lineNumber);
        return 0;
    }
    if (resolvedTokens.back().type == ParamValue::TYPE_STRING) {
        runtimeError("Expression cannot end with an operator: " + resolvedTokens.back().stringValue, lineNumber);
        return 0;
    }


    // 2. Pass 1: Perform Multiplication, Division, Modulo
    std::vector<ParamValue> pass1Result;
    for (size_t i = 0; i < resolvedTokens.size(); ++i) {
        const ParamValue& currentToken = resolvedTokens[i];

        if (currentToken.type == ParamValue::TYPE_STRING && (currentToken.stringValue == "*" || currentToken.stringValue == "/" || currentToken.stringValue == "%")) {
            // High precedence operator found
            if (pass1Result.empty() || pass1Result.back().type != ParamValue::TYPE_INT) {
                runtimeError("Invalid expression: Missing left operand for operator " + currentToken.stringValue, lineNumber);
                return 0;
            }
            // Check next token exists and is a number (right operand)
            if (i + 1 >= resolvedTokens.size() || resolvedTokens[i+1].type != ParamValue::TYPE_INT) {
                 runtimeError("Invalid expression: Missing right operand for operator " + currentToken.stringValue, lineNumber);
                 return 0;
            }

            int leftVal = pass1Result.back().intValue;
            pass1Result.pop_back(); // Remove left operand from result stack
            String op = currentToken.stringValue;
            int rightVal = resolvedTokens[i+1].intValue; // Get right operand from next token

            int result = applyOperation(leftVal, op, rightVal, lineNumber, this);
            pass1Result.push_back(ParamValue(result)); // Push result back onto stack

            i++; // IMPORTANT: Skip the right operand token in the next loop iteration
        } else {
            // Push numbers and low-precedence operators (+, -) onto the stack for Pass 2
             if (currentToken.type == ParamValue::TYPE_INT || (currentToken.type == ParamValue::TYPE_STRING && (currentToken.stringValue == "+" || currentToken.stringValue == "-"))) {
                pass1Result.push_back(currentToken);
             } else {
                 // Should be caught by earlier checks or parser, but defensive check
                 runtimeError("Invalid token encountered during Pass 1: " + currentToken.stringValue, lineNumber);
                 return 0;
             }
        }
    }

    // 3. Pass 2: Perform Addition, Subtraction (left-to-right)
    if (pass1Result.empty()) {
         // This can happen if the original expression was just a single number/variable
         // which was fully resolved in step 1. Check resolvedTokens size.
         if (resolvedTokens.size() == 1 && resolvedTokens[0].type == ParamValue::TYPE_INT) {
             return resolvedTokens[0].intValue;
         } else {
            // Or if Pass 1 resulted in an empty list due to an error caught above or unforeseen issue
            runtimeError("Expression evaluation failed (Pass 1 result empty unexpectedly).", lineNumber);
            return 0;
         }
    }

    // First element must be a number after pass 1
    if (pass1Result[0].type != ParamValue::TYPE_INT) {
         runtimeError("Invalid expression state after Pass 1 (does not start with number).", lineNumber);
         return 0;
    }

    int finalResult = pass1Result[0].intValue; // Start with the first number
    for (size_t i = 1; i < pass1Result.size(); i += 2) {
        // Expect Operator, Value pattern
        if (i + 1 >= pass1Result.size() || pass1Result[i].type != ParamValue::TYPE_STRING || pass1Result[i+1].type != ParamValue::TYPE_INT) {
             // Error in structure like "number operator non-number" or "number operator" at end
             runtimeError("Invalid expression structure during Pass 2 evaluation.", lineNumber);
             return 0;
        }
        String op = pass1Result[i].stringValue;
        int rightVal = pass1Result[i+1].intValue;

        // Only '+' and '-' should remain after Pass 1
        if (op != "+" && op != "-") {
             runtimeError("Internal error: Unexpected operator '" + op + "' during Pass 2.", lineNumber);
             return 0;
        }

        finalResult = applyOperation(finalResult, op, rightVal, lineNumber, this);
    }

    return finalResult;
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
    // We'll resolve parameters only when needed for each specific command type
    // instead of resolving all possible parameters for every command

    switch (cmd.type) {
        case CMD_VAR:
            // Initialize the declared variable. cmd.varName is UPPERCASE, no '$'
            if (!_variables.count(cmd.varName)) {
                if (!cmd.initialExpressionTokens.empty()) {
                    // Evaluate the initial expression
                    int initialValue = evaluateExpression(cmd.initialExpressionTokens, cmd.lineNumber);
                    _variables[cmd.varName] = initialValue;
                } else {
                    // No initializer, default to 0
                    _variables[cmd.varName] = 0;
                }
            } else {
                 // This case should ideally not happen if parser prevents re-declaration
                 runtimeError("Variable $" + cmd.varName + " already initialized.", cmd.lineNumber);
            }
            break;

        case CMD_LET:
             // Assignment: Evaluate expression and assign to target var
             // cmd.letTargetVar is UPPERCASE, no '$'
             if (_variables.count(cmd.letTargetVar)) {
                 if (!cmd.letExpressionTokens.empty()) {
                     int valueToAssign = evaluateExpression(cmd.letExpressionTokens, cmd.lineNumber);
                     _variables[cmd.letTargetVar] = valueToAssign;
                 } else {
                      // This should be caught by parser (missing expression)
                      runtimeError("Missing expression for LET statement.", cmd.lineNumber);
                 }
             } else {
                 // This should be caught by parser (undeclared var)
                 runtimeError("Attempted to assign to undeclared variable: $" + cmd.letTargetVar, cmd.lineNumber);
             }
            break;

        case CMD_COLOR: {
            // Pass line number to resolver
            String colorName = resolveStringParam("NAME", cmd.params, "BLACK", cmd.lineNumber);
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
             // Pass line number to resolver
            String fillName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber); // Handles SOLID or pattern name
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
        case CMD_TRANSLATE: {
            // Pass line number to resolver
            int dx = resolveIntParam("DX", cmd.params, 0, cmd.lineNumber);
            int dy = resolveIntParam("DY", cmd.params, 0, cmd.lineNumber);
            // Simplified: Absolute translation, not cumulative
            _currentState.translateX = dx;
            _currentState.translateY = dy;
            break;
        } // End CMD_TRANSLATE block
        case CMD_ROTATE: {
             // Pass line number to resolver
            int degrees = resolveIntParam("DEGREES", cmd.params, 0, cmd.lineNumber);
             // Simplified: Absolute rotation
            _currentState.rotationDegrees = degrees % 360;
             if (_currentState.rotationDegrees < 0) _currentState.rotationDegrees += 360;
            break;
        } // End CMD_ROTATE block
        case CMD_SCALE: {
             // Pass line number to resolver
            int factor = resolveIntParam("FACTOR", cmd.params, 1, cmd.lineNumber);
             // Simplified: Absolute scale
            _currentState.scale = (factor >= 1) ? factor : 1.0;
            break;
        } // End CMD_SCALE block

        // Drawing commands
        case CMD_PIXEL: {
             // Pass line number to resolver
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            _drawing.drawPixel(x, y, _currentState);
            break;
        }
        case CMD_FILL_PIXEL: {
              // Pass line number to resolver
             int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
             int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
             _drawing.drawFilledPixel(x, y, _currentState);
             break;
        }
        case CMD_LINE: {
             // Pass line number to resolver
            int x1 = resolveIntParam("X1", cmd.params, 0, cmd.lineNumber);
            int y1 = resolveIntParam("Y1", cmd.params, 0, cmd.lineNumber);
            int x2 = resolveIntParam("X2", cmd.params, 0, cmd.lineNumber);
            int y2 = resolveIntParam("Y2", cmd.params, 0, cmd.lineNumber);
            _drawing.drawLine(x1, y1, x2, y2, _currentState);
            break;
        }
        case CMD_RECT: {
             // Pass line number to resolver
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int w = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber);
            int h = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber);
            _drawing.drawRect(x, y, w, h, _currentState);
            break;
        }
        case CMD_FILL_RECT: {
             // Pass line number to resolver
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int w = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber);
            int h = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber);
            _drawing.fillRect(x, y, w, h, _currentState);
            break;
        }
        case CMD_CIRCLE: {
             // Pass line number to resolver
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int r = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber);
            _drawing.drawCircle(x, y, r, _currentState);
            break;
        }
        case CMD_FILL_CIRCLE: {
             // Pass line number to resolver
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int r = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber);
            _drawing.fillCircle(x, y, r, _currentState);
            break;
        }
        case CMD_DRAW: {
             // Pass line number to resolver
            String assetName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber);
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
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

        case CMD_REPEAT: {
            // Handle REPEAT command by executing nested commands in a loop
            int count = 0;
            if (cmd.count.type == ParamValue::TYPE_INT) {
                count = cmd.count.intValue;
            } else if (cmd.count.type == ParamValue::TYPE_VARIABLE) {
                // Resolve the variable to get the count, passing line number
                count = resolveValue(cmd.count, cmd.lineNumber);
            } else {
                runtimeError("Invalid REPEAT count type", cmd.lineNumber);
                break; // Exit case
            }

            if (count < 0) {
                runtimeError("REPEAT count cannot be negative: " + String(count), cmd.lineNumber);
                break; // Exit case
            }

            // Store previous $INDEX if nested loops
            int previousIndex = -1;
            bool hadPreviousIndex = _environment.count("$INDEX");
            if (hadPreviousIndex) {
                previousIndex = _environment["$INDEX"];
            }

            // Execute the nested commands 'count' times
            for (int i = 0; i < count; i++) {
                // Set the INDEX environment variable for this iteration
                _environment["$INDEX"] = i;

                // Execute each command in the nested block
                for (const auto& nestedCmd : cmd.nestedCommands) {
                    executeCommand(nestedCmd);
                }
            }

            // Restore previous $INDEX or remove it
            if (hadPreviousIndex) {
                 _environment["$INDEX"] = previousIndex;
            } else {
                 _environment.erase("$INDEX");
            }
            break; // End CMD_REPEAT block
        }

        case CMD_IF:
             runtimeError("IF command not implemented yet", cmd.lineNumber);
             break;

        case CMD_UNKNOWN:
        case CMD_DEFINE_PATTERN: // Handled by parser
        case CMD_NOOP:
        case CMD_ENDREPEAT: // Should be handled by parser, not runtime
            // Do nothing
            break;
    }
}