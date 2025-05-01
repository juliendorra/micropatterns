#include "micropatterns_parser.h"
#include <ctype.h> // For isdigit

MicroPatternsParser::MicroPatternsParser() {
    reset();
}

void MicroPatternsParser::reset() {
    _commands.clear();
    _assets.clear();
    _commandStack.clear(); // Clear the stack on reset
    _errors.clear();
    _declaredVariables.clear();
    _lineNumber = 0;
}

void MicroPatternsParser::addError(const String& message) {
    _errors.push_back("Line " + String(_lineNumber) + ": " + message);
}

const std::vector<MicroPatternsCommand>& MicroPatternsParser::getCommands() const {
    return _commands;
}

const std::map<String, MicroPatternsAsset>& MicroPatternsParser::getAssets() const {
    return _assets;
}

const std::vector<String>& MicroPatternsParser::getErrors() const {
    return _errors;
}

const std::vector<String>& MicroPatternsParser::getDeclaredVariables() const {
    return _declaredVariables;
}


bool MicroPatternsParser::parse(const String& scriptText) {
    reset();
    String currentLine;
    int start = 0;
    int end = scriptText.indexOf('\n');

    while (start < scriptText.length()) {
        _lineNumber++;
        if (end == -1) {
            currentLine = scriptText.substring(start);
        } else {
            currentLine = scriptText.substring(start, end);
        }
        currentLine.trim(); // Remove leading/trailing whitespace

        if (currentLine.length() > 0 && !currentLine.startsWith("#")) {
            if (!processLine(currentLine)) {
                // Stop parsing on first major error? Or collect all?
                // For now, let's collect all errors.
            }
        }

        if (end == -1) {
            break; // Reached end of script
        }
        start = end + 1;
        end = scriptText.indexOf('\n', start);
    }

    // After parsing all lines, check if any blocks are unclosed
    if (!_commandStack.empty()) {
        // Get the line number where the unclosed block started
        int startLine = _commandStack.back()->lineNumber;
        String blockType = (_commandStack.back()->type == CMD_REPEAT) ? "REPEAT" : "IF"; // Extend if IF is added
        addError("Unclosed " + blockType + " block started on line " + String(startLine) + ". Expected END" + blockType + ".");
    }


    return _errors.empty(); // Return true if no errors occurred
}

bool MicroPatternsParser::processLine(const String& line) {
    int firstSpace = line.indexOf(' ');
    String commandNameStr;
    String argsString = "";

    if (firstSpace == -1) {
        commandNameStr = line;
    } else {
        commandNameStr = line.substring(0, firstSpace);
        argsString = line.substring(firstSpace + 1);
        argsString.trim();
    }

    commandNameStr.toUpperCase(); // Commands are case-insensitive

    MicroPatternsCommand cmd;
    cmd.lineNumber = _lineNumber;
    bool isBlockStart = false; // Flag to indicate if this command starts a block

    // --- Handle specific commands ---
    // Handle block ends first
    if (commandNameStr == "ENDREPEAT") {
        if (_commandStack.empty() || _commandStack.back()->type != CMD_REPEAT) {
            addError("Unexpected ENDREPEAT without matching REPEAT.");
            return false;
        }
        _commandStack.pop_back(); // Pop the completed REPEAT block from the stack
        return true; // Don't add ENDREPEAT itself as a command object
    }
    // Add ENDIF handling here if implemented later...

    // Handle regular commands and block starts
    if (commandNameStr == "DEFINE") {
        // Expect "PATTERN NAME=..." after DEFINE
        String defineArgs = argsString;
        defineArgs.trim();
        String upperDefineArgs = defineArgs;
        upperDefineArgs.toUpperCase();
        if (!upperDefineArgs.startsWith("PATTERN ")) {
             addError("DEFINE command must be followed by 'PATTERN'.");
             return false;
        }
        // Extract arguments *after* "PATTERN "
        String patternArgs = defineArgs.substring(8); // Length of "PATTERN "
        patternArgs.trim();
        if (!parseDefinePattern(patternArgs)) return false;
        // DEFINE PATTERN is handled entirely at parse time, no command object needed.
        return true;
    } else if (commandNameStr == "VAR") {
        String varName;
        std::vector<ParamValue> tokens;
        if (!parseVar(argsString, varName, tokens)) {
            return false; // Error already added by parseVar
        }
        // Create command object *after* successful parsing
        cmd.type = CMD_VAR;
        cmd.varName = varName; // Store parsed name (UPPERCASE, no '$')
        cmd.initialExpressionTokens = tokens; // Store parsed tokens
        // Let the standard logic below add the command to the list
    } else if (commandNameStr == "LET") {
        String targetVar;
        std::vector<ParamValue> tokens;
        if (!parseLet(argsString, targetVar, tokens)) {
            return false; // Error already added by parseLet
        }
        // Create command object *after* successful parsing
        cmd.type = CMD_LET;
        cmd.letTargetVar = targetVar; // Store parsed target name (UPPERCASE, no '$')
        cmd.letExpressionTokens = tokens; // Store parsed tokens
        // Let the standard logic below add the command to the list
    } else if (commandNameStr == "REPEAT") {
        ParamValue countVal;
        if (!parseRepeat(argsString, countVal)) {
             return false; // Error already added by parseRepeat
        }
        // Create command object *after* successful parsing
        cmd.type = CMD_REPEAT;
        cmd.count = countVal; // Store parsed count value
        isBlockStart = true; // Mark this as a block-starting command
        // Let the standard logic below add the command to the list
    } else if (commandNameStr == "COLOR") {
        cmd.type = CMD_COLOR;
    } else if (commandNameStr == "FILL") {
        cmd.type = CMD_FILL;
    } else if (commandNameStr == "DRAW") {
        cmd.type = CMD_DRAW;
    } else if (commandNameStr == "RESET_TRANSFORMS") {
        cmd.type = CMD_RESET_TRANSFORMS;
    } else if (commandNameStr == "TRANSLATE") {
        cmd.type = CMD_TRANSLATE;
    } else if (commandNameStr == "ROTATE") {
        cmd.type = CMD_ROTATE;
    } else if (commandNameStr == "SCALE") {
        cmd.type = CMD_SCALE;
    } else if (commandNameStr == "PIXEL") {
        cmd.type = CMD_PIXEL;
    } else if (commandNameStr == "FILL_PIXEL") {
        cmd.type = CMD_FILL_PIXEL;
    } else if (commandNameStr == "LINE") {
        cmd.type = CMD_LINE;
    } else if (commandNameStr == "RECT") {
        cmd.type = CMD_RECT;
    } else if (commandNameStr == "FILL_RECT") {
        cmd.type = CMD_FILL_RECT;
    } else if (commandNameStr == "CIRCLE") {
        cmd.type = CMD_CIRCLE;
    } else if (commandNameStr == "FILL_CIRCLE") {
        cmd.type = CMD_FILL_CIRCLE;
    } else {
        addError("Unknown command: " + commandNameStr);
        return false;
    }

    // Parse parameters for commands that use the generic KEY=VALUE format
    // Skip for commands with special syntax handled above (DEFINE, VAR, LET, REPEAT)
    // Also skip for commands with no parameters (RESET_TRANSFORMS)
    if (cmd.type != CMD_DEFINE_PATTERN && cmd.type != CMD_VAR && cmd.type != CMD_LET &&
        cmd.type != CMD_REPEAT && cmd.type != CMD_RESET_TRANSFORMS && cmd.type != CMD_NOOP)
    {
        if (!parseParams(argsString, cmd.params)) {
            // Error already added in parseParams
            return false;
        }
    }

    // Add the command to the correct list (top-level or nested)
    if (_commandStack.empty()) {
        // Add to top-level list
        _commands.push_back(cmd);
        // If this command starts a block, push its pointer (from the main list) onto the stack
        if (isBlockStart) {
            _commandStack.push_back(&_commands.back());
        }
    } else {
        // Add to the nested command list of the currently open block
        MicroPatternsCommand* currentBlock = _commandStack.back();
        currentBlock->nestedCommands.push_back(cmd);
        // If this nested command also starts a block, push its pointer (from the nested list) onto the stack
        if (isBlockStart) {
            _commandStack.push_back(&currentBlock->nestedCommands.back());
        }
    }

    return true;
}

// Parses REPEAT COUNT=value TIMES
// Populates outCount with the parsed value (int or variable)
bool MicroPatternsParser::parseRepeat(const String& argsString, ParamValue& outCount) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();
    String upperArgs = trimmedArgs; // Create copy for case-insensitive checks
    upperArgs.toUpperCase();

    // Check for COUNT= part
    if (!upperArgs.startsWith("COUNT=")) {
        addError("REPEAT requires COUNT= parameter.");
        return false;
    }

    // Find the end of the COUNT value and the start of TIMES
    int equalsPos = trimmedArgs.indexOf('='); // Should be 5 if startsWith is true
    int timesPos = upperArgs.indexOf(" TIMES"); // Find " TIMES" case-insensitively

    if (timesPos == -1) {
        addError("REPEAT requires TIMES keyword after COUNT value.");
        return false;
    }

    // Extract the value string between '=' and 'TIMES'
    String countValueStr = trimmedArgs.substring(equalsPos + 1, timesPos);
    countValueStr.trim();

    if (countValueStr.length() == 0) {
        addError("Missing value for REPEAT COUNT.");
        return false;
    }

    // Check for extra characters after TIMES
    String remaining = trimmedArgs.substring(timesPos + 6); // Length of " TIMES"
    remaining.trim();
    if (remaining.length() > 0) {
        addError("Unexpected characters after TIMES in REPEAT command: '" + remaining + "'");
        return false;
    }

    // Parse the count value (can be int or variable)
    outCount = parseValue(countValueStr);

    // Validate count type (must be int or variable)
    if (outCount.type != ParamValue::TYPE_INT && outCount.type != ParamValue::TYPE_VARIABLE) {
        addError("REPEAT COUNT value must be an integer or a variable ($var). Got: " + countValueStr);
        return false;
    }
    // Further validation (e.g., non-negative) happens at runtime

    return true;
}

// Parses the arguments for DEFINE PATTERN NAME=... WIDTH=... HEIGHT=... DATA=...
bool MicroPatternsParser::parseDefinePattern(const String& argsString) {
    // argsString contains everything after "DEFINE PATTERN "

    // This map will store the parsed parameters for this specific command
    std::map<String, ParamValue> patternParams;
    // Pass the already trimmed argsString directly to parseParams
    if (!parseParams(argsString, patternParams)) {
        return false; // Error already added by parseParams
    }

    // Validate required params using the local patternParams map
    if (patternParams.find("NAME") == patternParams.end() || patternParams["NAME"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires NAME=\"...\" parameter.");
        return false;
    }
    if (patternParams.find("WIDTH") == patternParams.end() || patternParams["WIDTH"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires WIDTH=... parameter.");
        return false;
    }
    if (patternParams.find("HEIGHT") == patternParams.end() || patternParams["HEIGHT"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires HEIGHT=... parameter.");
        return false;
    }
    if (patternParams.find("DATA") == patternParams.end() || patternParams["DATA"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires DATA=\"...\" parameter.");
        return false;
    }

    MicroPatternsAsset asset;
    // Retrieve the unquoted string value parsed by parseParams/parseValue
    String originalName = patternParams["NAME"].stringValue;
    asset.name = originalName; // Store original case name for potential display
    asset.name.toUpperCase(); // Use uppercase name as the key for the assets map
    asset.width = patternParams["WIDTH"].intValue;
    asset.height = patternParams["HEIGHT"].intValue;
    String dataStr = patternParams["DATA"].stringValue; // Get unquoted data string

    if (asset.width <= 0 || asset.height <= 0) {
        addError("Pattern WIDTH and HEIGHT must be positive.");
        return false;
    }

    int expectedLen = asset.width * asset.height;
    if (dataStr.length() != expectedLen) {
        // Simple handling: error out. Could also pad/truncate.
        addError("DATA length (" + String(dataStr.length()) + ") does not match WIDTH*HEIGHT (" + String(expectedLen) + ").");
        return false;
    }

    for (int i = 0; i < dataStr.length(); ++i) {
        if (dataStr[i] == '0') {
            asset.data.push_back(0);
        } else if (dataStr[i] == '1') {
            asset.data.push_back(1);
        } else {
            addError("DATA string must contain only '0' or '1'.");
            return false;
        }
    }

    if (_assets.count(asset.name)) {
        // Use the original case name retrieved earlier for the error message
        addError("Pattern '" + originalName + "' already defined.");
        return false;
    }

    _assets[asset.name] = asset;
    return true;
}

// Parses VAR $name [= expression]
// Populates outVarName (UPPERCASE, no '$') and outTokens (parsed expression, empty if no init)
bool MicroPatternsParser::parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens) {
    outTokens.clear(); // Ensure output tokens are empty initially
    String trimmedArgs = argsString;
    trimmedArgs.trim();

    if (!trimmedArgs.startsWith("$")) {
        addError("VAR requires a variable name starting with '$'.");
        return false;
    }

    // Determine where the variable name ends and if there's an expression
    int spacePos = trimmedArgs.indexOf(' ');
    int equalsPos = trimmedArgs.indexOf('=');
    int nameEndPos = trimmedArgs.length();
    String expressionPart = "";

    if (equalsPos != -1) {
        // Found '=', potential expression
        nameEndPos = equalsPos; // Name ends before '='
        expressionPart = trimmedArgs.substring(equalsPos + 1);
        expressionPart.trim();
        if (expressionPart.length() == 0) {
            addError("Missing expression after '=' in VAR declaration.");
            return false;
        }
    } else if (spacePos != -1) {
        // Found space but no '=', invalid syntax like "VAR $x 10"
        addError("Invalid VAR syntax. Use 'VAR $name' or 'VAR $name = expression'.");
        return false;
    }
    // If neither '=' nor space found, nameEndPos remains length(), expressionPart remains ""

    // Extract and validate name
    outVarName = trimmedArgs.substring(1, nameEndPos); // Extract name after '$'
    outVarName.trim();
    outVarName.toUpperCase();

    if (outVarName.length() == 0) {
        addError("Invalid variable name in VAR declaration.");
        return false;
    }

    // Check for re-declaration (case-insensitive)
    for(const String& existingVar : _declaredVariables) {
        if (existingVar.equalsIgnoreCase(outVarName)) {
            addError("Variable '$" + outVarName + "' already declared.");
            return false;
        }
    }

    // Check against environment variables (case-insensitive)
    String testName = "$" + outVarName;
    if (testName.equalsIgnoreCase("$HOUR") || testName.equalsIgnoreCase("$MINUTE") ||
        testName.equalsIgnoreCase("$SECOND") || testName.equalsIgnoreCase("$COUNTER") ||
        testName.equalsIgnoreCase("$WIDTH") || testName.equalsIgnoreCase("$HEIGHT") ||
        testName.equalsIgnoreCase("$INDEX")) {
        addError("Cannot declare variable with the same name as an environment variable: $" + outVarName);
        return false;
    }

    // Parse expression if present
    if (expressionPart.length() > 0) {
        if (!parseExpression(expressionPart, outTokens)) {
            // Error already added by parseExpression
            return false;
        }
    }
    // If no expression, outTokens remains empty.

    // Add variable to declared list *after* all checks pass
    _declaredVariables.push_back(outVarName);

    return true;
}


// Parses LET $name = expression
// Populates outTargetVarName (UPPERCASE, no '$') and outTokens (parsed expression)
bool MicroPatternsParser::parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens) {
     outTokens.clear(); // Ensure output tokens are empty initially
     String trimmedArgs = argsString;
     trimmedArgs.trim();

     int equalsPos = trimmedArgs.indexOf('=');
     if (equalsPos == -1) {
         addError("LET statement requires '=' for assignment.");
         return false;
     }

     String targetVarStr = trimmedArgs.substring(0, equalsPos);
     targetVarStr.trim();
     String expressionStr = trimmedArgs.substring(equalsPos + 1);
     expressionStr.trim();

     if (!targetVarStr.startsWith("$")) {
         addError("LET target variable must start with '$'.");
         return false;
     }
     if (expressionStr.length() == 0) {
         addError("LET statement requires an expression after '='.");
         return false;
     }

     outTargetVarName = targetVarStr.substring(1);
     outTargetVarName.toUpperCase();

     // Check if variable was declared
     bool declared = false;
     for(const String& existingVar : _declaredVariables) {
         if (existingVar.equalsIgnoreCase(outTargetVarName)) {
             declared = true;
             break;
         }
     }
     if (!declared) {
         addError("Cannot assign to undeclared variable: $" + outTargetVarName);
         return false;
     }

    // Parse the expression and store tokens in the output parameter
    if (!parseExpression(expressionStr, outTokens)) {
        // Error already added by parseExpression
        return false;
    }

    // Check if expression is empty (should be caught by parseExpression, but double-check)
    if (outTokens.empty()) {
        addError("Internal parser error: LET expression parsed to empty token list.");
        return false;
    }

    return true;
}


// Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into the params map
bool MicroPatternsParser::parseParams(const String& argsString, std::map<String, ParamValue>& params) {
    String remainingArgs = argsString;
    remainingArgs.trim();
    const char* ptr = remainingArgs.c_str(); // Use C-style pointer for easier manipulation

    while (*ptr != '\0') {
        // Skip leading whitespace
        while (*ptr != '\0' && isspace(*ptr)) {
            ptr++;
        }
        if (*ptr == '\0') break; // End of string

        // Find KEY
        const char* keyStart = ptr;
        while (*ptr != '\0' && *ptr != '=' && !isspace(*ptr)) {
            ptr++;
        }
        String key(keyStart, ptr - keyStart);
        key.toUpperCase(); // Parameter names are case-insensitive

        if (key.length() == 0) {
            addError("Empty parameter name found near '" + String(keyStart) + "'.");
            return false;
        }

        // Skip whitespace after key
        while (*ptr != '\0' && isspace(*ptr)) {
            ptr++;
        }

        // Expect '='
        if (*ptr != '=') {
            addError("Missing '=' after parameter name '" + key + "'.");
            return false;
        }
        ptr++; // Skip '='

        // Skip whitespace after '='
        while (*ptr != '\0' && isspace(*ptr)) {
            ptr++;
        }

        if (*ptr == '\0') {
            addError("Missing value for parameter '" + key + "'.");
            return false;
        }

        // Find VALUE
        String valueString;
        if (*ptr == '"') {
            // Quoted string value
            ptr++; // Skip opening quote
            const char* valueStart = ptr;
            while (*ptr != '\0' && *ptr != '"') {
                // Basic handling: doesn't support escaped quotes within
                ptr++;
            }
            if (*ptr != '"') {
                addError("Unterminated string literal for parameter '" + key + "'.");
                return false;
            }
            valueString = String(valueStart, ptr - valueStart);
            params[key] = ParamValue(valueString, false); // Store unquoted string
            ptr++; // Skip closing quote
        } else {
            // Unquoted value (number, variable, keyword)
            const char* valueStart = ptr;
            while (*ptr != '\0' && !isspace(*ptr)) {
                // Value ends at the next space
                ptr++;
            }
            valueString = String(valueStart, ptr - valueStart);
            if (valueString.length() == 0) {
                 addError("Missing value for parameter '" + key + "'.");
                 return false;
            }
            params[key] = parseValue(valueString); // ParseValue determines type
        }

        // Parameter already exists? (Shouldn't happen with map if keys are unique)
        // if (params.count(key)) { ... } // Map handles overwriting, but maybe add warning?

        // Skip whitespace before next potential parameter
        while (*ptr != '\0' && isspace(*ptr)) {
            ptr++;
        }
    } // End while loop

    return true;
}

// Parses a single value string (which might be quoted or unquoted)
ParamValue MicroPatternsParser::parseValue(const String& valueString) {
    // Check if it's intended as a quoted string (already unquoted by parseParams)
    // This function now receives the *content* of the value.
    // parseParams handles the quotes.

    if (valueString.startsWith("$")) {
        // Variable reference
        if (valueString.length() <= 1) {
             addError("Invalid variable format: '$' must be followed by a name.");
             return ParamValue(0); // Return dummy value on error
        }
        // Store the variable name including '$', uppercase for runtime resolution
        String upperVarName = valueString;
        upperVarName.toUpperCase();
        return ParamValue(upperVarName, true); // Mark as variable type
    }

    // Check if it's an integer
    bool isNegative = false;
    int startIndex = 0;
    if (valueString.startsWith("-")) {
        isNegative = true;
        startIndex = 1;
    }
    bool allDigits = true;
    if (valueString.length() == startIndex) { // Handle just "-"
        allDigits = false;
    }
    for (int i = startIndex; i < valueString.length(); ++i) {
        if (!isdigit(valueString[i])) {
            allDigits = false;
            break;
        }
    }

    if (allDigits) {
        return ParamValue(valueString.toInt());
    }

    // Otherwise, treat as a string (e.g., keyword like BLACK, WHITE, SOLID)
    // Runtime will handle validation of keywords.
    return ParamValue(valueString, false);
}

// Parses an expression string like "10 + $VAR * 2" into tokens.
// Operators (+, -, *, /, %) are stored as TYPE_STRING.
// Numbers are TYPE_INT. Variables are TYPE_VARIABLE.
bool MicroPatternsParser::parseExpression(const String& expressionString, std::vector<ParamValue>& tokens) {
    tokens.clear();
    String currentToken;
    enum State { NONE, NUMBER, VARIABLE, OPERATOR } state = NONE;

    for (int i = 0; i < expressionString.length(); ++i) {
        char c = expressionString[i];

        if (isspace(c)) {
            if (currentToken.length() > 0) {
                tokens.push_back(parseValue(currentToken)); // Parse the completed token
                currentToken = "";
                state = NONE;
            }
            continue; // Skip whitespace
        }

        switch (state) {
            case NONE:
                if (isdigit(c) || (c == '-' && (i + 1 < expressionString.length() && isdigit(expressionString[i+1])))) {
                    state = NUMBER;
                    currentToken += c;
                } else if (c == '$') {
                    state = VARIABLE;
                    currentToken += c;
                } else if (String("+-*/%").indexOf(c) != -1) {
                    // Treat operator as its own token immediately
                    tokens.push_back(ParamValue(String(c), false)); // Store operator as string
                    state = NONE; // Reset state after operator
                } else {
                    addError("Invalid character in expression: '" + String(c) + "'");
                    return false;
                }
                break;

            case NUMBER:
                if (isdigit(c)) {
                    currentToken += c;
                } else if (String("+-*/%").indexOf(c) != -1) {
                    // End of number, start of operator
                    tokens.push_back(parseValue(currentToken)); // Add number token
                    tokens.push_back(ParamValue(String(c), false)); // Add operator token
                    currentToken = "";
                    state = NONE;
                } else {
                    addError("Invalid character after number in expression: '" + String(c) + "'");
                    return false;
                }
                break;

            case VARIABLE:
                // Allow letters, numbers, underscore after '$'
                if (isalnum(c) || c == '_') {
                    currentToken += c;
                } else if (String("+-*/%").indexOf(c) != -1) {
                    // End of variable, start of operator
                    tokens.push_back(parseValue(currentToken)); // Add variable token
                    tokens.push_back(ParamValue(String(c), false)); // Add operator token
                    currentToken = "";
                    state = NONE;
                } else {
                    addError("Invalid character after variable name in expression: '" + String(c) + "'");
                    return false;
                }
                break;

            case OPERATOR: // Operators are handled immediately, so this state isn't really used
                 addError("Internal parser error: Unexpected OPERATOR state.");
                 return false;
        }
    }

    // Add the last token if any
    if (currentToken.length() > 0) {
        tokens.push_back(parseValue(currentToken));
    }

    // Basic validation: Ensure alternating value/operator pattern (simplified check)
    if (tokens.empty()) {
        addError("Empty expression.");
        return false;
    }
    bool expectValue = true;
    for (const auto& token : tokens) {
        bool isValue = (token.type == ParamValue::TYPE_INT || token.type == ParamValue::TYPE_VARIABLE);
        bool isOperator = (token.type == ParamValue::TYPE_STRING && String("+-*/%").indexOf(token.stringValue) != -1);

        if (expectValue && !isValue) {
            addError("Syntax error in expression: Expected number or variable, found '" + token.stringValue + "'.");
            return false;
        }
        if (!expectValue && !isOperator) {
             addError("Syntax error in expression: Expected operator (+-*/%), found value.");
             return false;
        }
        if (!isValue && !isOperator) {
             addError("Syntax error in expression: Invalid token '" + token.stringValue + "'.");
             return false;
        }
        expectValue = !expectValue; // Toggle expectation
    }
    if (expectValue) { // Must end with a value
        addError("Syntax error in expression: Cannot end with an operator.");
        return false;
    }


    // Validate that all variables used exist
    for (const auto& token : tokens) {
        if (token.type == ParamValue::TYPE_VARIABLE) {
            String varNameUpper = token.stringValue; // Includes '$'
            varNameUpper.toUpperCase();
            String bareNameUpper = varNameUpper.substring(1);

            bool declared = false;
            for(const String& existingVar : _declaredVariables) {
                if (existingVar.equalsIgnoreCase(bareNameUpper)) {
                    declared = true;
                    break;
                }
            }
            bool isEnv = (varNameUpper == "$HOUR" || varNameUpper == "$MINUTE" || varNameUpper == "$SECOND" ||
                          varNameUpper == "$COUNTER" || varNameUpper == "$WIDTH" || varNameUpper == "$HEIGHT" ||
                          varNameUpper == "$INDEX");

            if (!declared && !isEnv) {
                addError("Undefined variable used in expression: " + token.stringValue);
                return false;
            }
        }
    }


    return true;
}