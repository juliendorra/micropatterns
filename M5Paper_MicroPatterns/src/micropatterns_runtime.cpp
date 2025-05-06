#include "micropatterns_runtime.h"
#include "esp32-hal-log.h" // For logging errors
#include <Arduino.h>       // For yield()

// Define colors (consistent with drawing class)
const uint8_t RUNTIME_COLOR_WHITE = 0;
const uint8_t RUNTIME_COLOR_BLACK = 15;

MicroPatternsRuntime::MicroPatternsRuntime(M5EPD_Canvas *canvas, const std::map<String, MicroPatternsAsset> &assets)
    : _canvas(canvas), _drawing(canvas), _assets(assets)
{
    resetState();
    // Initialize environment variables that don't change per run
    _environment["$WIDTH"] = _canvas ? _canvas->width() : 540;   // Default or actual
    _environment["$HEIGHT"] = _canvas ? _canvas->height() : 960; // Default or actual
    // Initialize time/counter to 0, will be set externally
    _environment["$HOUR"] = 0;
    _environment["$MINUTE"] = 0;
    _environment["$SECOND"] = 0;
    _environment["$COUNTER"] = 0;
    // $INDEX is managed during execution
}

void MicroPatternsRuntime::setCommands(const std::vector<MicroPatternsCommand> *commands)
{
    _commands = commands;
}

void MicroPatternsRuntime::setDeclaredVariables(const std::set<String> *declaredVariables)
{
    _declaredVariables = declaredVariables;
}

void MicroPatternsRuntime::resetState()
{
    _currentState = MicroPatternsState(); // Reset drawing state (color, fill, transforms)
    _variables.clear();                   // Clear user variables
    _environment.erase("$INDEX");         // Ensure $INDEX is cleared initially

    // Re-initialize environment variables that might change per run (counter, time)
    // Note: $WIDTH/$HEIGHT are set once in constructor.

    // Environment values for time and counter need to be set via setCounter/setTime before execute()
    // Counter is preserved  across resets and managed by the system
    // Hour, Minute and Second are based on the RTC
}

void MicroPatternsRuntime::setCounter(int counter)
{
    log_d("Runtime setCounter: %d", counter);
    _environment["$COUNTER"] = counter;
}

void MicroPatternsRuntime::setTime(int hour, int minute, int second)
{
    // Basic validation could be added here
    _environment["$HOUR"] = hour;
    _environment["$MINUTE"] = minute;
    _environment["$SECOND"] = second;
}

void MicroPatternsRuntime::runtimeError(const String &message, int lineNumber)
{
    // Simple error logging for now
    log_e("Runtime Error (Line %d): %s", lineNumber, message.c_str());
    // Could potentially halt execution or store errors
}

// --- Parameter Resolution ---

// Resolves a single value (int literal or variable reference)
// Uses lineNumber for error reporting. loopIndex provides context for $INDEX.
int MicroPatternsRuntime::resolveValue(const ParamValue &val, int lineNumber, int loopIndex)
{
    if (val.type == ParamValue::TYPE_INT)
    {
        return val.intValue;
    }
    else if (val.type == ParamValue::TYPE_VARIABLE)
    {
        // Variable names in ParamValue are stored including '$', case preserved from parsing
        String varName = val.stringValue;
        varName.toUpperCase(); // Use uppercase for lookup (ensures case-insensitivity)

        // Special handling for $INDEX (case-insensitive)
        if (varName == "$INDEX")
        {
            if (loopIndex < 0)
            {
                runtimeError("Variable $INDEX can only be used inside a REPEAT loop.", lineNumber);
                return 0; // Default value on error
            }
            return loopIndex; // Return the current loop index directly
        }

        // Check environment variables (keys are like "$COUNTER")
        if (_environment.count(varName))
        {
            int value = _environment.at(varName);
            if (varName == "$COUNTER")
            {
                log_d("Runtime resolveValue accessing $COUNTER: %d", value);
            }

            if (varName == "$SECOND")
            {
                log_d("Runtime resolveValue accessing $SECOND: %d", value);
            }
            if (varName == "$MINUTE")
            {
                log_d("Runtime resolveValue accessing $MINUTE: %d", value);
            }

            if (varName == "$HOUR")
            {
                log_d("Runtime resolveValue accessing $HOUR: %d", value);
            }

            return value;
        }

        // Check user variables (keys are like "$MYVAR")
        if (_variables.count(varName))
        {
            return _variables.at(varName);
        }
        else
        {
            // This should ideally be caught by parser, but check defensively
            runtimeError("Undefined variable: " + val.stringValue, lineNumber);
            return 0; // Default value on error
        }
    }
    else
    {
        // String literal or other type - cannot resolve to int directly
        runtimeError("Expected integer or variable, got: " + val.stringValue, lineNumber);
        return 0; // Default value on error
    }
}

int MicroPatternsRuntime::resolveIntParam(const String &paramName, const std::map<String, ParamValue> &params, int defaultValue, int lineNumber)
{
    String upperParamName = paramName;
    upperParamName.toUpperCase(); // Ensure lookup is case-insensitive

    if (params.count(upperParamName))
    {
        const ParamValue &val = params.at(upperParamName);
        if (val.type == ParamValue::TYPE_INT || val.type == ParamValue::TYPE_VARIABLE)
        {
            // Resolve the value, passing loopIndex = -1 as params aren't inside loops directly
            return resolveValue(val, lineNumber, -1);
        }
        else
        {
            runtimeError("Parameter " + paramName + " requires an integer or variable, got string: " + val.stringValue, lineNumber);
            return defaultValue;
        }
    }
    // Parameter not found - return default. This is not an error.
    return defaultValue;
}

String MicroPatternsRuntime::resolveStringParam(const String &paramName, const std::map<String, ParamValue> &params, const String &defaultValue, int lineNumber)
{
    String upperParamName = paramName;
    upperParamName.toUpperCase(); // Ensure case-insensitive parameter name lookup

    if (params.count(upperParamName))
    {
        const ParamValue &val = params.at(upperParamName);
        // Expect TYPE_STRING for keywords like BLACK/WHITE or unquoted pattern names from parseValue
        if (val.type == ParamValue::TYPE_STRING)
        {
            return val.stringValue; // Return the parsed string value
        }
        else
        {
            runtimeError("Parameter " + paramName + " requires a string or keyword. Got type " + String(val.type), lineNumber);
            return defaultValue;
        }
    }
    // Return default value for missing parameters (optional parameters)
    return defaultValue;
}

// Handles NAME=SOLID or NAME="pattern"
// Returns "SOLID" or the UPPERCASE pattern name string.
String MicroPatternsRuntime::resolveAssetNameParam(const String &paramName, const std::map<String, ParamValue> &params, int lineNumber)
{
    String upperParamName = paramName;
    upperParamName.toUpperCase(); // Ensure case-insensitive parameter name lookup

    if (params.count(upperParamName))
    {
        const ParamValue &val = params.at(upperParamName);
        // Expect TYPE_STRING which holds either the keyword "SOLID" or the pattern name
        if (val.type == ParamValue::TYPE_STRING)
        {
            String nameValue = val.stringValue;
            nameValue.toUpperCase(); // Ensure case-insensitive comparison/lookup
            if (nameValue == "SOLID")
            {
                return "SOLID"; // Return normalized keyword
            }
            else
            {
                // Return the pattern name (already uppercase from string conversion)
                return nameValue;
            }
        }
        else
        {
            runtimeError("Parameter " + paramName + " requires SOLID or a pattern name string. Got type " + String(val.type), lineNumber);
            return "SOLID"; // Default to solid on error
        }
    }
    // Return default value for missing parameters (optional parameters)
    return "SOLID"; // Default to solid if NAME param is missing
}

// --- Expression & Condition Evaluation ---

// Helper to apply operation, returns result or logs error and returns 0
int applyOperation(int val1, const String &op, int val2, int lineNumber, MicroPatternsRuntime *runtime)
{
    if (op == "+")
        return val1 + val2;
    if (op == "-")
        return val1 - val2;
    if (op == "*")
        return val1 * val2;
    if (op == "/")
    {
        if (val2 == 0)
        {
            runtime->runtimeError("Division by zero.", lineNumber);
            return 0;
        }
        // Integer division truncates towards zero
        return val1 / val2;
    }
    if (op == "%")
    {
        if (val2 == 0)
        {
            runtime->runtimeError("Modulo by zero.", lineNumber);
            return 0;
        }
        // C++ % handles negative numbers correctly for this purpose
        return val1 % val2;
    }
    runtime->runtimeError("Unknown operator: " + op, lineNumber);
    return 0;
}

// Evaluates a sequence of expression tokens (numbers, variables, operators)
int MicroPatternsRuntime::evaluateExpression(const std::vector<ParamValue> &tokens, int lineNumber, int loopIndex)
{
    if (tokens.empty())
    {
        // VAR $x = ; results in empty tokens. Default to 0.
        return 0;
    }

    // 1. Resolve all variables to numbers, keep operators
    std::vector<ParamValue> resolvedTokens;
    for (const auto &token : tokens)
    {
        if (token.type == ParamValue::TYPE_VARIABLE)
        {
            resolvedTokens.push_back(ParamValue(resolveValue(token, lineNumber, loopIndex)));
        }
        else if (token.type == ParamValue::TYPE_INT || token.type == ParamValue::TYPE_OPERATOR)
        {
            resolvedTokens.push_back(token);
        }
        else
        {
            runtimeError("Unexpected token type during expression resolution: " + token.stringValue, lineNumber);
            return 0;
        }
    }

    // Basic structural checks after resolution
    if (resolvedTokens.empty())
    {
        runtimeError("Expression resolved to empty token list.", lineNumber);
        return 0;
    }
    // Check for operator at start/end or consecutive operators/values
    bool expectValue = true;
    for (size_t i = 0; i < resolvedTokens.size(); ++i)
    {
        bool isValue = resolvedTokens[i].type == ParamValue::TYPE_INT;
        bool isOperator = resolvedTokens[i].type == ParamValue::TYPE_OPERATOR;
        if (expectValue && !isValue)
        {
            runtimeError("Syntax error in expression: Expected number, found operator '" + resolvedTokens[i].stringValue + "'.", lineNumber);
            return 0;
        }
        if (!expectValue && !isOperator)
        {
            runtimeError("Syntax error in expression: Expected operator, found number '" + String(resolvedTokens[i].intValue) + "'.", lineNumber);
            return 0;
        }
        expectValue = !expectValue;
    }
    if (expectValue)
    { // Must end with a value
        runtimeError("Syntax error in expression: Cannot end with an operator.", lineNumber);
        return 0;
    }

    // 2. Pass 1: Perform Multiplication, Division, Modulo
    std::vector<ParamValue> pass1Result;
    for (size_t i = 0; i < resolvedTokens.size(); ++i)
    {
        const ParamValue &currentToken = resolvedTokens[i];

        if (currentToken.type == ParamValue::TYPE_OPERATOR && (currentToken.stringValue == "*" || currentToken.stringValue == "/" || currentToken.stringValue == "%"))
        {
            if (pass1Result.empty() || pass1Result.back().type != ParamValue::TYPE_INT)
            {
                runtimeError("Invalid expression: Missing left operand for operator " + currentToken.stringValue, lineNumber);
                return 0;
            }
            if (i + 1 >= resolvedTokens.size() || resolvedTokens[i + 1].type != ParamValue::TYPE_INT)
            {
                runtimeError("Invalid expression: Missing right operand for operator " + currentToken.stringValue, lineNumber);
                return 0;
            }

            int leftVal = pass1Result.back().intValue;
            pass1Result.pop_back();
            String op = currentToken.stringValue;
            int rightVal = resolvedTokens[i + 1].intValue;

            int result = applyOperation(leftVal, op, rightVal, lineNumber, this);
            pass1Result.push_back(ParamValue(result));

            i++; // Skip the right operand token
        }
        else
        {
            // Push numbers and low-precedence operators (+, -)
            if (currentToken.type == ParamValue::TYPE_INT || (currentToken.type == ParamValue::TYPE_OPERATOR && (currentToken.stringValue == "+" || currentToken.stringValue == "-")))
            {
                pass1Result.push_back(currentToken);
            }
            else
            {
                runtimeError("Invalid token encountered during Pass 1: " + currentToken.stringValue, lineNumber);
                return 0;
            }
        }
    }

    // 3. Pass 2: Perform Addition, Subtraction (left-to-right)
    if (pass1Result.empty())
    {
        if (resolvedTokens.size() == 1 && resolvedTokens[0].type == ParamValue::TYPE_INT)
        {
            return resolvedTokens[0].intValue; // Single value expression
        }
        else
        {
            runtimeError("Expression evaluation failed (Pass 1 result empty unexpectedly).", lineNumber);
            return 0;
        }
    }
    if (pass1Result[0].type != ParamValue::TYPE_INT)
    {
        runtimeError("Invalid expression state after Pass 1 (does not start with number).", lineNumber);
        return 0;
    }

    int finalResult = pass1Result[0].intValue;
    for (size_t i = 1; i < pass1Result.size(); i += 2)
    {
        if (i + 1 >= pass1Result.size() || pass1Result[i].type != ParamValue::TYPE_OPERATOR || pass1Result[i + 1].type != ParamValue::TYPE_INT)
        {
            runtimeError("Invalid expression structure during Pass 2 evaluation.", lineNumber);
            return 0;
        }
        String op = pass1Result[i].stringValue;
        int rightVal = pass1Result[i + 1].intValue;

        if (op != "+" && op != "-")
        {
            runtimeError("Internal error: Unexpected operator '" + op + "' during Pass 2.", lineNumber);
            return 0;
        }
        finalResult = applyOperation(finalResult, op, rightVal, lineNumber, this);
    }

    return finalResult;
}

// Evaluates a sequence of condition tokens
bool MicroPatternsRuntime::evaluateCondition(const std::vector<ParamValue> &tokens, int lineNumber, int loopIndex)
{
    if (tokens.empty())
    {
        runtimeError("Cannot evaluate empty condition.", lineNumber);
        return false;
    }

    // Conditions can be complex, involving arithmetic before comparison.
    // Example: $X + 10 > $Y / 2
    // Example: $COUNTER % 10 == 0

    // For simplicity, let's assume the structure is either:
    // 1. value op value (where op is comparison)
    // 2. value % literal op value (where op is comparison)

    // Find the comparison operator
    int comparisonOpIndex = -1;
    String comparisonOp = "";
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].type == ParamValue::TYPE_OPERATOR)
        {
            String opStr = tokens[i].stringValue;
            if (opStr == "==" || opStr == "!=" || opStr == "<" || opStr == ">" || opStr == "<=" || opStr == ">=")
            {
                if (comparisonOpIndex != -1)
                {
                    runtimeError("Multiple comparison operators found in condition.", lineNumber);
                    return false;
                }
                comparisonOpIndex = i;
                comparisonOp = opStr;
            }
        }
    }

    if (comparisonOpIndex == -1)
    {
        runtimeError("No comparison operator (==, !=, <, >, <=, >=) found in condition.", lineNumber);
        return false;
    }

    // Split tokens into left and right expressions based on the comparison operator
    std::vector<ParamValue> leftTokens(tokens.begin(), tokens.begin() + comparisonOpIndex);
    std::vector<ParamValue> rightTokens(tokens.begin() + comparisonOpIndex + 1, tokens.end());

    if (leftTokens.empty() || rightTokens.empty())
    {
        runtimeError("Missing left or right side of comparison in condition.", lineNumber);
        return false;
    }

    // Evaluate left and right expressions
    int leftValue = evaluateExpression(leftTokens, lineNumber, loopIndex);
    int rightValue = evaluateExpression(rightTokens, lineNumber, loopIndex);

    // Perform comparison
    if (comparisonOp == "==")
        return leftValue == rightValue;
    if (comparisonOp == "!=")
        return leftValue != rightValue;
    if (comparisonOp == "<")
        return leftValue < rightValue;
    if (comparisonOp == ">")
        return leftValue > rightValue;
    if (comparisonOp == "<=")
        return leftValue <= rightValue;
    if (comparisonOp == ">=")
        return leftValue >= rightValue;

    // Should not reach here
    runtimeError("Unknown comparison operator: " + comparisonOp, lineNumber);
    return false;
}

// --- Execution ---

void MicroPatternsRuntime::execute()
{
    if (!_commands || !_canvas || !_declaredVariables)
    {
        log_e("Runtime not properly initialized (commands, canvas, or declared vars missing).");
        return;
    }

    // Reset state before each full execution (clears user vars, resets drawing state, preserves counter)
    resetState();
    log_d("Runtime execute start - Counter after resetState: %d", _environment["$COUNTER"]);

    // Initialize declared variables to 0 (or evaluated expression)
    // This happens *before* executing commands.
    // Note: resetState() clears _variables, so we need to re-initialize here.
    for (const auto &cmd : *_commands)
    {
        if (cmd.type == CMD_VAR)
        {
            // Initialize the declared variable. cmd.varName is UPPERCASE, no '$'
            String varKey = "$" + cmd.varName; // Store with '$' prefix (case-insensitive)
            if (!_variables.count(varKey))
            { // Check if already initialized (shouldn't happen with reset)
                if (!cmd.initialExpressionTokens.empty())
                {
                    // Evaluate the initial expression (loopIndex = -1 for top-level)
                    int initialValue = evaluateExpression(cmd.initialExpressionTokens, cmd.lineNumber, -1);
                    _variables[varKey] = initialValue;
                }
                else
                {
                    // No initializer, default to 0
                    _variables[varKey] = 0;
                }
            }
            else
            {
                runtimeError("Variable " + varKey + " already initialized (internal error).", cmd.lineNumber);
            }
        }
    }

    _drawing.clearCanvas(); // Start with a clear (white) canvas

    // Execute top-level commands
    int commandCounter = 0;
    for (const auto &cmd : *_commands)
    {
        // Skip VAR commands as they were handled above for initialization
        if (cmd.type != CMD_VAR)
        {
            executeCommand(cmd, -1); // Pass loopIndex = -1 for top-level execution
            commandCounter++;
            if (commandCounter > 0 && commandCounter % 20 == 0) { // Yield every 20 top-level commands
                yield();
            }
        }
    }

    // After executing all commands, push the final canvas to the display
    _canvas->pushCanvas(0, 0, UPDATE_MODE_GLD16);
}

// Execute a single command, potentially recursively for blocks
void MicroPatternsRuntime::executeCommand(const MicroPatternsCommand &cmd, int loopIndex)
{
    // loopIndex is >= 0 if inside a REPEAT loop, -1 otherwise.

    // Resolve parameters only when needed for each specific command type
    // Pass current loopIndex to resolvers

    try
    { // Add try-catch around command execution
        switch (cmd.type)
        {
        case CMD_VAR:
            // Initialization is handled before main execution loop in execute()
            break;

        case CMD_LET:
        {
            // Assignment: Evaluate expression and assign to target var
            // cmd.letTargetVar is UPPERCASE, no '$'
            String targetVarKey = "$" + cmd.letTargetVar; // Lookup/store with '$'
            if (_variables.count(targetVarKey))
            {
                if (!cmd.letExpressionTokens.empty())
                {
                    int valueToAssign = evaluateExpression(cmd.letExpressionTokens, cmd.lineNumber, loopIndex);
                    _variables[targetVarKey] = valueToAssign;
                }
                else
                {
                    runtimeError("Missing expression for LET statement.", cmd.lineNumber);
                }
            }
            else
            {
                // Should be caught by parser (undeclared var)
                runtimeError("Attempted to assign to undeclared variable: " + targetVarKey, cmd.lineNumber);
            }
            break;
        } // End CMD_LET block

        case CMD_COLOR:
        {
            String colorName = resolveStringParam("NAME", cmd.params, "BLACK", cmd.lineNumber);
            colorName.toUpperCase(); // Ensure case-insensitive comparison
            if (colorName == "WHITE")
                _currentState.color = RUNTIME_COLOR_WHITE;
            else if (colorName == "BLACK")
                _currentState.color = RUNTIME_COLOR_BLACK;
            else
                runtimeError("Invalid COLOR NAME: " + colorName, cmd.lineNumber);
            break;
        }
        case CMD_FILL:
        {
            String fillName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber); // Handles SOLID or pattern name (UPPERCASE)
            if (fillName == "SOLID")
            {
                _currentState.fillAsset = nullptr;
            }
            else
            {
                if (_assets.count(fillName))
                {
                    _currentState.fillAsset = &_assets.at(fillName);
                }
                else
                {
                    // Use original name from asset if possible for error
                    String originalName = fillName; // Fallback
                    for (const auto &pair : _assets)
                    {
                        if (pair.first == fillName)
                        {
                            originalName = pair.second.originalName;
                            break;
                        }
                    }
                    runtimeError("Undefined fill pattern: \"" + originalName + "\"", cmd.lineNumber);
                    _currentState.fillAsset = nullptr; // Default to solid on error
                }
            }
            break;
        }
        case CMD_RESET_TRANSFORMS:
            _currentState.scale = 1.0;
            _currentState.transformations.clear();
            break;
        case CMD_TRANSLATE:
        {
            int dx = resolveIntParam("DX", cmd.params, 0, cmd.lineNumber);
            int dy = resolveIntParam("DY", cmd.params, 0, cmd.lineNumber);
            _currentState.transformations.emplace_back(TransformOp::TRANSLATE, dx, dy);
            break;
        }
        case CMD_ROTATE:
        {
            int degrees = resolveIntParam("DEGREES", cmd.params, 0, cmd.lineNumber);
            // Store the absolute rotation degrees for this operation
            _currentState.transformations.emplace_back(TransformOp::ROTATE, degrees);
            break;
        }
        case CMD_SCALE:
        {
            int factor = resolveIntParam("FACTOR", cmd.params, 1, cmd.lineNumber);
            // Set the absolute scale factor
            _currentState.scale = (factor >= 1) ? factor : 1.0;
            break;
        }

        // Drawing commands
        case CMD_PIXEL:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            _drawing.drawPixel(x, y, _currentState);
            break;
        }
        case CMD_FILL_PIXEL:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            _drawing.drawFilledPixel(x, y, _currentState);
            break;
        }
        case CMD_LINE:
        {
            int x1 = resolveIntParam("X1", cmd.params, 0, cmd.lineNumber);
            int y1 = resolveIntParam("Y1", cmd.params, 0, cmd.lineNumber);
            int x2 = resolveIntParam("X2", cmd.params, 0, cmd.lineNumber);
            int y2 = resolveIntParam("Y2", cmd.params, 0, cmd.lineNumber);
            _drawing.drawLine(x1, y1, x2, y2, _currentState);
            break;
        }
        case CMD_RECT:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int w = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber);
            int h = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber);
            _drawing.drawRect(x, y, w, h, _currentState);
            break;
        }
        case CMD_FILL_RECT:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int w = resolveIntParam("WIDTH", cmd.params, 0, cmd.lineNumber);
            int h = resolveIntParam("HEIGHT", cmd.params, 0, cmd.lineNumber);
            _drawing.fillRect(x, y, w, h, _currentState);
            break;
        }
        case CMD_CIRCLE:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int r = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber);
            _drawing.drawCircle(x, y, r, _currentState);
            break;
        }
        case CMD_FILL_CIRCLE:
        {
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            int r = resolveIntParam("RADIUS", cmd.params, 0, cmd.lineNumber);
            _drawing.fillCircle(x, y, r, _currentState);
            break;
        }
        case CMD_DRAW:
        {
            String assetName = resolveAssetNameParam("NAME", cmd.params, cmd.lineNumber); // UPPERCASE name
            int x = resolveIntParam("X", cmd.params, 0, cmd.lineNumber);
            int y = resolveIntParam("Y", cmd.params, 0, cmd.lineNumber);
            if (assetName != "SOLID")
            {
                if (_assets.count(assetName))
                {
                    _drawing.drawAsset(x, y, _assets.at(assetName), _currentState);
                }
                else
                {
                    String originalName = assetName; // Fallback
                    for (const auto &pair : _assets)
                    {
                        if (pair.first == assetName)
                        {
                            originalName = pair.second.originalName;
                            break;
                        }
                    }
                    runtimeError("Undefined asset for DRAW: \"" + originalName + "\"", cmd.lineNumber);
                }
            }
            else
            {
                runtimeError("Cannot DRAW SOLID.", cmd.lineNumber);
            }
            break;
        }

        case CMD_REPEAT:
        {
            int count = resolveValue(cmd.count, cmd.lineNumber, loopIndex); // Resolve count using current context

            if (count < 0)
            {
                runtimeError("REPEAT count cannot be negative: " + String(count), cmd.lineNumber);
                break;
            }

            // Store previous $INDEX if nested loops
            int previousIndex = -1;
            bool hadPreviousIndex = _environment.count("$INDEX");
            if (hadPreviousIndex)
            {
                previousIndex = _environment.at("$INDEX");
            }

            // Execute the nested commands 'count' times
            for (int i = 0; i < count; i++)
            {
                // Set the INDEX environment variable for this iteration
                // This makes $INDEX available both via direct environment lookup
                // and via the loopIndex parameter in resolveValue
                _environment["$INDEX"] = i;

                // Execute each command in the nested block, passing current loop index 'i'
                for (const auto &nestedCmd : cmd.nestedCommands)
                {
                    executeCommand(nestedCmd, i); // Pass 'i' as loopIndex
                }
                // Yield periodically within the REPEAT loop
                if (i > 0 && i % 10 == 0) { // Yield every 10 iterations (but not the first)
                    yield();
                }
            }

            // Restore previous $INDEX or remove it
            if (hadPreviousIndex)
            {
                _environment["$INDEX"] = previousIndex;
            }
            else
            {
                _environment.erase("$INDEX");
            }
            break; // End CMD_REPEAT block
        }

        case CMD_IF:
        {
            bool conditionMet = evaluateCondition(cmd.conditionTokens, cmd.lineNumber, loopIndex);
            const auto &commandsToRun = conditionMet ? cmd.thenCommands : cmd.elseCommands;

            // Execute the chosen block of commands
            for (const auto &nestedCmd : commandsToRun)
            {
                // Pass down the current loopIndex if IF is inside REPEAT
                executeCommand(nestedCmd, loopIndex);
            }
            break; // End CMD_IF block
        }

        case CMD_UNKNOWN:
        case CMD_DEFINE_PATTERN: // Handled by parser
        case CMD_NOOP:
        case CMD_ENDREPEAT: // Should not be executed
        case CMD_ELSE:      // Should not be executed
        case CMD_ENDIF:     // Should not be executed
            // Do nothing or log internal error if these are encountered
            break;
        } // End switch
    }
    catch (...)
    {
        // Catch potential exceptions during command execution (e.g., from drawing)
        runtimeError("Unexpected exception during command execution.", cmd.lineNumber);
        // Potentially rethrow or handle differently
    }
}