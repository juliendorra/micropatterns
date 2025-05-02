#include "micropatterns_parser.h"
#include <ctype.h> // For isdigit, isspace, isalnum
#include "esp32-hal-log.h" // For log_w warning

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

const std::set<String>& MicroPatternsParser::getDeclaredVariables() const {
    return _declaredVariables;
}

// Helper to check if a variable name (UPPERCASE, no '$') is an environment variable
bool MicroPatternsParser::isEnvVar(const String& upperCaseName) const {
    return upperCaseName == "HOUR" || upperCaseName == "MINUTE" || upperCaseName == "SECOND" ||
           upperCaseName == "COUNTER" || upperCaseName == "WIDTH" || upperCaseName == "HEIGHT" ||
           upperCaseName == "INDEX";
}

// Helper to validate variable usage in expressions/conditions
// Expects varRefWithDollar like "$MYVAR" (case might vary)
bool MicroPatternsParser::validateVariableUsage(const String& varRefWithDollar) {
    if (!varRefWithDollar.startsWith("$") || varRefWithDollar.length() <= 1) {
        addError("Invalid variable reference format: " + varRefWithDollar);
        return false;
    }
    String bareNameUpper = varRefWithDollar.substring(1);
    bareNameUpper.toUpperCase();

    if (!isEnvVar(bareNameUpper) && _declaredVariables.find(bareNameUpper) == _declaredVariables.end()) {
        addError("Undefined variable used: " + varRefWithDollar);
        return false;
    }
    return true;
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
        MicroPatternsCommand* openBlock = _commandStack.back();
        int startLine = openBlock->lineNumber;
        String blockType = (openBlock->type == CMD_REPEAT) ? "REPEAT" : "IF";
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
    bool isBlockEnd = false;   // Flag for ENDREPEAT/ENDIF
    bool isElse = false;       // Flag for ELSE

    // --- Handle block ends and ELSE first ---
    if (commandNameStr == "ENDREPEAT") {
        if (_commandStack.empty() || _commandStack.back()->type != CMD_REPEAT) {
            addError("Unexpected ENDREPEAT without matching REPEAT.");
            return false;
        }
        isBlockEnd = true;
    } else if (commandNameStr == "ENDIF") {
         if (_commandStack.empty() || _commandStack.back()->type != CMD_IF) {
            addError("Unexpected ENDIF without matching IF.");
            return false;
        }
        isBlockEnd = true;
    } else if (commandNameStr == "ELSE") {
        if (_commandStack.empty() || _commandStack.back()->type != CMD_IF) {
            addError("Unexpected ELSE without matching IF.");
            return false;
        }
        MicroPatternsCommand* currentIf = _commandStack.back();
        // Check if ELSE already encountered for this IF
        if (!currentIf->elseCommands.empty() || (currentIf->params.count("_processingElse") && currentIf->params["_processingElse"].intValue == 1)) {
             addError("Multiple ELSE clauses for the same IF statement (started on line " + String(currentIf->lineNumber) + ").");
             return false;
        }
        // Mark the IF command on the stack as processing the ELSE part
        // Use a dummy param in the map for this internal state
        currentIf->params["_processingElse"] = ParamValue(1); // Mark as processing else
        isElse = true;
        // ELSE itself doesn't generate a command object to be added to lists
        return true;
    }

    // If it's a block end, pop the stack and return the completed command object
    if (isBlockEnd) {
        MicroPatternsCommand* completedBlock = _commandStack.back();
        _commandStack.pop_back();
        // Add the completed block to the parent's list or top-level list
        if (_commandStack.empty()) {
            _commands.push_back(*completedBlock);
        } else {
            _commandStack.back()->nestedCommands.push_back(*completedBlock);
             // If the parent was an IF, add to the correct branch
             if (_commandStack.back()->type == CMD_IF) {
                 MicroPatternsCommand* parentIf = _commandStack.back();
                 if (parentIf->params.count("_processingElse") && parentIf->params["_processingElse"].intValue == 1) {
                     parentIf->elseCommands.push_back(*completedBlock);
                 } else {
                     parentIf->thenCommands.push_back(*completedBlock);
                 }
             } else { // Parent is REPEAT
                 _commandStack.back()->nestedCommands.push_back(*completedBlock);
             }
        }
        // We don't need the pointer anymore, but the object is copied.
        // delete completedBlock; // Don't delete, it was allocated on stack or in vector
        return true; // Don't add ENDREPEAT/ENDIF itself as a command object
    }


    // --- Handle regular commands and block starts ---
    if (commandNameStr == "DEFINE") {
        String defineArgs = argsString;
        defineArgs.trim();
        String upperDefineArgs = defineArgs;
        upperDefineArgs.toUpperCase();
        if (!upperDefineArgs.startsWith("PATTERN ")) {
             addError("DEFINE command must be followed by 'PATTERN'.");
             return false;
        }
        String patternArgs = defineArgs.substring(8); // Length of "PATTERN "
        patternArgs.trim();
        if (!parseDefinePattern(patternArgs)) return false;
        cmd.type = CMD_NOOP; // Handled at parse time
    } else if (commandNameStr == "VAR") {
        String varName;
        std::vector<ParamValue> tokens;
        if (!parseVar(argsString, varName, tokens)) return false;
        cmd.type = CMD_VAR;
        cmd.varName = varName;
        cmd.initialExpressionTokens = tokens;
    } else if (commandNameStr == "LET") {
        String targetVar;
        std::vector<ParamValue> tokens;
        if (!parseLet(argsString, targetVar, tokens)) return false;
        cmd.type = CMD_LET;
        cmd.letTargetVar = targetVar;
        cmd.letExpressionTokens = tokens;
    } else if (commandNameStr == "REPEAT") {
        ParamValue countVal;
        if (!parseRepeat(argsString, countVal)) return false;
        cmd.type = CMD_REPEAT;
        cmd.count = countVal;
        isBlockStart = true;
    } else if (commandNameStr == "IF") {
        std::vector<ParamValue> conditionTokens;
        if (!parseIf(argsString, conditionTokens)) return false;
        cmd.type = CMD_IF;
        cmd.conditionTokens = conditionTokens;
        isBlockStart = true;
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
    // Skip for commands with special syntax handled above or no params
    if (cmd.type != CMD_DEFINE_PATTERN && cmd.type != CMD_VAR && cmd.type != CMD_LET &&
        cmd.type != CMD_REPEAT && cmd.type != CMD_IF && cmd.type != CMD_RESET_TRANSFORMS &&
        cmd.type != CMD_NOOP)
    {
        if (!parseParams(argsString, cmd.params)) {
            // Error already added in parseParams
            return false;
        }
    }

    // Add the command to the correct list (top-level or nested)
    if (cmd.type != CMD_NOOP) { // Don't add NOOP commands
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
             // If the parent is IF, add to the correct branch
             if (currentBlock->type == CMD_IF) {
                 if (currentBlock->params.count("_processingElse") && currentBlock->params["_processingElse"].intValue == 1) {
                     currentBlock->elseCommands.push_back(cmd);
                     if (isBlockStart) {
                         _commandStack.push_back(&currentBlock->elseCommands.back());
                     }
                 } else {
                     currentBlock->thenCommands.push_back(cmd);
                      if (isBlockStart) {
                         _commandStack.push_back(&currentBlock->thenCommands.back());
                     }
                 }
             } else { // Parent is REPEAT
                 currentBlock->nestedCommands.push_back(cmd);
                 if (isBlockStart) {
                     _commandStack.push_back(&currentBlock->nestedCommands.back());
                 }
             }
        }
    }

    return true;
}

// Parses REPEAT COUNT=value TIMES
bool MicroPatternsParser::parseRepeat(const String& argsString, ParamValue& outCount) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();
    String upperArgs = trimmedArgs;
    upperArgs.toUpperCase();

    if (!upperArgs.startsWith("COUNT=")) {
        addError("REPEAT requires COUNT= parameter.");
        return false;
    }

    int equalsPos = trimmedArgs.indexOf('=');
    int timesPos = upperArgs.indexOf(" TIMES");

    if (timesPos == -1) {
        addError("REPEAT requires TIMES keyword after COUNT value.");
        return false;
    }

    String countValueStr = trimmedArgs.substring(equalsPos + 1, timesPos);
    countValueStr.trim();

    if (countValueStr.length() == 0) {
        addError("Missing value for REPEAT COUNT.");
        return false;
    }

    String remaining = trimmedArgs.substring(timesPos + 6); // Length of " TIMES"
    remaining.trim();
    if (remaining.length() > 0) {
        addError("Unexpected characters after TIMES in REPEAT command: '" + remaining + "'");
        return false;
    }

    outCount = parseValue(countValueStr);

    if (outCount.type != ParamValue::TYPE_INT && outCount.type != ParamValue::TYPE_VARIABLE) {
        addError("REPEAT COUNT value must be an integer or a variable ($var). Got: " + countValueStr);
        return false;
    }
    if (outCount.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(outCount.stringValue)) {
        return false; // Error added by validateVariableUsage
    }

    return true;
}

// Parses IF condition THEN
bool MicroPatternsParser::parseIf(const String& argsString, std::vector<ParamValue>& outConditionTokens) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();
    String upperArgs = trimmedArgs;
    upperArgs.toUpperCase();

    int thenPos = upperArgs.lastIndexOf(" THEN");
    if (thenPos == -1 || thenPos != upperArgs.length() - 5) { // Check it's at the very end
        addError("IF requires ' THEN' at the end of the condition.");
        return false;
    }

    String conditionStr = trimmedArgs.substring(0, thenPos);
    conditionStr.trim();

    if (conditionStr.length() == 0) {
        addError("Missing condition for IF statement.");
        return false;
    }

    // Parse the condition string into tokens
    if (!parseCondition(conditionStr, outConditionTokens)) {
        // Error added by parseCondition
        return false;
    }

    return true;
}


// Parses the arguments for DEFINE PATTERN NAME=... WIDTH=... HEIGHT=... DATA=...
bool MicroPatternsParser::parseDefinePattern(const String& argsString) {
    std::map<String, ParamValue> patternParams;
    if (!parseParams(argsString, patternParams)) {
        return false; // Error already added by parseParams
    }

    // Validate required params
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
    asset.originalName = patternParams["NAME"].stringValue; // Store original case name
    asset.name = asset.originalName;
    asset.name.toUpperCase(); // Use uppercase name as the key for case-insensitive lookup
    asset.width = patternParams["WIDTH"].intValue;
    asset.height = patternParams["HEIGHT"].intValue;
    String dataStr = patternParams["DATA"].stringValue; // Get unquoted data string

    if (asset.width <= 0 || asset.height <= 0) {
        addError("Pattern WIDTH and HEIGHT must be positive.");
        return false;
    }
    if (asset.width > 20 || asset.height > 20) {
         log_w("Line %d: Pattern '%s' dimensions (%dx%d) exceed recommended maximum (20x20).", _lineNumber, asset.originalName.c_str(), asset.width, asset.height);
    }

    int expectedLen = asset.width * asset.height;
    if (dataStr.length() != expectedLen) {
        String action = (dataStr.length() < expectedLen) ? "padded with '0'" : "truncated";
        log_w("Line %d: DATA length (%d) for pattern '%s' does not match WIDTH*HEIGHT (%d). Data will be %s.",
              _lineNumber, dataStr.length(), asset.originalName.c_str(), expectedLen, action.c_str());

        if (dataStr.length() < expectedLen) {
            while (dataStr.length() < expectedLen) {
                dataStr += '0';
            }
        } else {
            dataStr = dataStr.substring(0, expectedLen);
        }
    }

    asset.data.reserve(expectedLen);
    for (int i = 0; i < dataStr.length(); ++i) {
        if (dataStr[i] == '0') {
            asset.data.push_back(0);
        } else if (dataStr[i] == '1') {
            asset.data.push_back(1);
        } else {
            addError("DATA string must contain only '0' or '1'. Found '" + String(dataStr[i]) + "' in pattern '" + asset.originalName + "'.");
            return false;
        }
    }

    if (_assets.count(asset.name)) {
        addError("Pattern '" + asset.originalName + "' (or equivalent case) already defined.");
        return false;
    }
    if (_assets.size() >= 16) {
         addError("Maximum number of defined patterns (16) reached.");
         return false;
    }

    _assets[asset.name] = asset;
    return true;
}

// Parses VAR $name [= expression]
bool MicroPatternsParser::parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens) {
    outTokens.clear();
    String trimmedArgs = argsString;
    trimmedArgs.trim();

    if (!trimmedArgs.startsWith("$")) {
        addError("VAR requires a variable name starting with '$'.");
        return false;
    }

    int equalsPos = trimmedArgs.indexOf('=');
    int nameEndPos = trimmedArgs.length();
    String expressionPart = "";

    if (equalsPos != -1) {
        nameEndPos = equalsPos;
        expressionPart = trimmedArgs.substring(equalsPos + 1);
        expressionPart.trim();
        if (expressionPart.length() == 0) {
            addError("Missing expression after '=' in VAR declaration.");
            return false;
        }
    } else {
        // Check for trailing characters after name if no '='
        int spacePos = trimmedArgs.indexOf(' ');
        if (spacePos != -1) {
             String extraContent = trimmedArgs.substring(spacePos);
             extraContent.trim(); // Trim in place
             addError("Invalid VAR syntax. Use 'VAR $name' or 'VAR $name = expression'. Found extra content: '" + extraContent + "'");
             return false;
        }
    }

    String varRef = trimmedArgs.substring(0, nameEndPos);
    varRef.trim();
    if (varRef.length() <= 1) { // Just "$"
        addError("Invalid variable name '$' in VAR declaration.");
        return false;
    }
    outVarName = varRef.substring(1); // Extract name after '$'
    outVarName.toUpperCase(); // Ensure case-insensitive variable names

    if (outVarName.length() == 0) { // Should be caught by length check, but defensive
        addError("Invalid variable name in VAR declaration.");
        return false;
    }
    // Check for valid characters (basic check)
    for (char c : outVarName) {
        if (!isalnum(c) && c != '_') {
             addError("Invalid character '" + String(c) + "' in variable name: " + varRef);
             return false;
        }
    }


    if (_declaredVariables.count(outVarName)) {
        addError("Variable '" + varRef + "' (or equivalent case) already declared.");
        return false;
    }
    if (isEnvVar(outVarName)) {
        addError("Cannot declare variable with the same name as an environment variable: " + varRef);
        return false;
    }

    // Add variable to declared list *before* parsing expression
    _declaredVariables.insert(outVarName);

    if (expressionPart.length() > 0) {
        if (!parseExpression(expressionPart, outTokens)) {
            // Error already added by parseExpression
            // Remove the variable from declared list if expression parsing failed? No, keep it declared.
            return false;
        }
    }

    return true;
}


// Parses LET $name = expression
bool MicroPatternsParser::parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens) {
     outTokens.clear();
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

     if (!targetVarStr.startsWith("$") || targetVarStr.length() <= 1) {
         addError("LET target variable must start with '$' followed by a name.");
         return false;
     }
     if (expressionStr.length() == 0) {
         addError("LET statement requires an expression after '='.");
         return false;
     }

     outTargetVarName = targetVarStr.substring(1);
     outTargetVarName.toUpperCase(); // Ensure case-insensitive variable names

     // Check if variable was declared (case-insensitive)
     if (_declaredVariables.find(outTargetVarName) == _declaredVariables.end()) {
         addError("Cannot assign to undeclared variable: " + targetVarStr);
         return false;
     }
     // Check if trying to assign to an environment variable
     if (isEnvVar(outTargetVarName)) {
          addError("Cannot assign to environment variable: " + targetVarStr);
          return false;
     }

    if (!parseExpression(expressionStr, outTokens)) {
        // Error already added by parseExpression
        return false;
    }
    if (outTokens.empty()) { // Should be caught by parseExpression
        addError("Internal parser error: LET expression parsed to empty token list.");
        return false;
    }

    return true;
}


// Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into the params map
// Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into the params map
bool MicroPatternsParser::parseParams(const String& argsString, std::map<String, ParamValue>& params) {
    String remainingArgs = argsString;
    remainingArgs.trim();
    const char* ptr = remainingArgs.c_str();

    while (*ptr != '\0') {
        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr == '\0') break;

        const char* keyStart = ptr;
        while (*ptr != '\0' && *ptr != '=' && !isspace(*ptr)) ptr++;
        String key(keyStart, ptr - keyStart);
        key.toUpperCase(); // Ensure case-insensitive parameter names

        if (key.length() == 0) {
            addError("Empty parameter name found near '" + String(keyStart) + "'.");
            return false;
        }

        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr != '=') {
            addError("Missing '=' after parameter name '" + key + "'.");
            return false;
        }
        ptr++; // Skip '='
        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr == '\0') {
            addError("Missing value for parameter '" + key + "'.");
            return false;
        }

        String valueString;
        ParamValue::ValueType valueType = ParamValue::TYPE_STRING; // Default assumption

        if (*ptr == '"') {
            ptr++; // Skip opening quote
            const char* valueStart = ptr;
            String tempVal = "";
            while (*ptr != '\0') {
                if (*ptr == '"') {
                    break; // Found closing quote
                }
                if (*ptr == '\\' && *(ptr + 1) != '\0') { // Handle escapes
                    ptr++; // Move to the escaped character
                    if (*ptr == '"' || *ptr == '\\') {
                        tempVal += *ptr; // Add escaped quote or backslash
                    } else {
                        // Keep the backslash and the character if it's not a known escape
                        tempVal += '\\';
                        tempVal += *ptr;
                    }
                } else {
                    tempVal += *ptr;
                }
                ptr++;
            }

            if (*ptr != '"') {
                addError("Unterminated string literal for parameter '" + key + "'.");
                return false;
            }
            valueString = tempVal; // Store unquoted, unescaped string
            valueType = ParamValue::TYPE_STRING;
            ptr++; // Skip closing quote
        } else {
            // Unquoted value (number, variable, keyword)
            const char* valueStart = ptr;
            // Value ends at the next space unless it's part of the next KEY=
            while (*ptr != '\0' && !isspace(*ptr)) {
                 ptr++;
            }
            // Look ahead to see if the next non-space is KEY=
            const char* lookahead = ptr;
            while (*lookahead != '\0' && isspace(*lookahead)) lookahead++;
            const char* nextKeyStart = lookahead;
            while (*lookahead != '\0' && *lookahead != '=' && !isspace(*lookahead)) lookahead++;
            bool nextIsKey = (*lookahead == '=');

            // If the next thing is not KEY=, the space might be part of the value (e.g., in conditions)
            // However, for standard params, values don't contain spaces.
            // Let parseValue handle the type detection.
            valueString = String(valueStart, ptr - valueStart);
            if (valueString.length() == 0) {
                 addError("Missing value for parameter '" + key + "'.");
                 return false;
            }
            // parseValue determines type (INT, VARIABLE, STRING keyword)
            ParamValue parsedVal = parseValue(valueString);
            valueString = parsedVal.stringValue; // May contain $VAR or keyword
            valueType = parsedVal.type;
            // If it was parsed as int, store int value
            if (valueType == ParamValue::TYPE_INT) {
                 params[key] = ParamValue(parsedVal.intValue);
                 continue; // Skip storing stringValue below
            }
        }

        if (params.count(key)) {
             addError("Duplicate parameter: " + key);
             return false;
        }
        params[key] = ParamValue(valueString, valueType);

        while (*ptr != '\0' && isspace(*ptr)) ptr++;
    }

    return true;
}

// Parses a single unquoted value string. Determines if it's an integer, variable, or keyword string.
ParamValue MicroPatternsParser::parseValue(const String& valueString) {
    if (valueString.startsWith("$")) {
        if (valueString.length() <= 1 || !isalpha(valueString[1])) { // Must start with $ followed by letter
             // Error will be caught later by validateVariableUsage if needed
             // Return as string for now
             return ParamValue(valueString, ParamValue::TYPE_STRING);
        }
        // Store the variable name including '$', case preserved for potential errors,
        // but runtime/validation should use uppercase.
        return ParamValue(valueString, ParamValue::TYPE_VARIABLE);
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
        // Use strtol for robust conversion and range check potential
        char* endptr;
        long val = strtol(valueString.c_str(), &endptr, 10);
        if (*endptr == '\0') { // Ensure entire string was consumed
             // Check if value fits in int (Arduino int is usually 16 or 32 bit)
             if (val >= INT_MIN && val <= INT_MAX) {
                 return ParamValue((int)val);
             } else {
                  addError("Integer value out of range: " + valueString);
                  return ParamValue(0); // Return dummy value
             }
        }
        // If strtol failed to parse the whole string as integer, fall through to string
    }

    // Otherwise, treat as a string (e.g., keyword like BLACK, WHITE, SOLID)
    // Runtime will handle validation of keywords.
    return ParamValue(valueString, ParamValue::TYPE_STRING);
}

// Parses an expression string like "10 + $VAR * 2" into tokens.
// Operators (+, -, *, /, %) are stored as TYPE_OPERATOR.
// Numbers are TYPE_INT. Variables are TYPE_VARIABLE.
// Revised implementation to fix state management bugs.
bool MicroPatternsParser::parseExpression(const String& expressionString, std::vector<ParamValue>& tokens) {
    tokens.clear();
    String currentToken;
    enum Expectation { EXPECT_VALUE, EXPECT_OPERATOR } expected = EXPECT_VALUE;
    bool unaryMinusPossible = true; // Allow unary minus at start or after operator

    for (int i = 0; i < expressionString.length(); ++i) {
        char c = expressionString[i];

        if (isspace(c)) {
            continue; // Skip whitespace
        }

        // --- Try parsing a value (number or variable) ---
        // Check for start of value: '$', digit, or '-' if unary is possible
        if ( (c == '$') || isdigit(c) || (c == '-' && unaryMinusPossible) ) {
            if (expected != EXPECT_VALUE) {
                addError("Syntax error in expression: Unexpected value '" + String(c) + "...'. Expected operator.");
                return false;
            }

            currentToken = "";
            // Handle potential unary minus
            if (c == '-') {
                currentToken += c;
                i++; // Move past '-'
                // Ensure something follows the unary minus
                if (i >= expressionString.length()) {
                     addError("Syntax error in expression: Incomplete expression after unary '-'.");
                     return false;
                }
                // Ensure the character after '-' is a digit or '$'
                if (!isdigit(expressionString[i]) && expressionString[i] != '$') {
                     addError("Syntax error in expression: Invalid character after unary '-'. Expected digit or '$'.");
                     return false;
                }
                c = expressionString[i]; // Get the next char (digit or '$')
            }

            // Parse Variable
            if (c == '$') {
                currentToken += c;
                i++; // Move past '$'
                // Ensure variable name starts correctly
                if (i >= expressionString.length() || !isalpha(expressionString[i])) {
                     addError("Syntax error in expression: Expected letter after '$'.");
                     return false;
                }
                currentToken += expressionString[i]; // Add first letter
                i++;
                // Consume the rest of the variable name
                while (i < expressionString.length() && (isalnum(expressionString[i]) || expressionString[i] == '_')) {
                    currentToken += expressionString[i];
                    i++;
                }
                i--; // Decrement because the outer loop will increment

                ParamValue val = parseValue(currentToken); // parseValue handles $VARNAME format
                if (val.type != ParamValue::TYPE_VARIABLE) { // Should be variable
                     addError("Internal parser error: Expected variable token for '" + currentToken + "'.");
                     return false;
                }
                 if (!validateVariableUsage(val.stringValue)) return false;
                 tokens.push_back(val);

            // Parse Number (potentially after unary minus)
            } else if (isdigit(c)) {
                 currentToken += c; // Add first digit
                 i++;
                 // Consume the rest of the digits
                 while (i < expressionString.length() && isdigit(expressionString[i])) {
                     currentToken += expressionString[i];
                     i++;
                 }
                 i--; // Decrement because the outer loop will increment

                 ParamValue val = parseValue(currentToken); // parseValue handles numbers
                 if (val.type != ParamValue::TYPE_INT) { // Should be int
                      addError("Internal parser error: Expected integer token for '" + currentToken + "'.");
                      return false;
                 }
                 tokens.push_back(val);
            } else {
                 // This state should not be reachable due to the checks after unary minus
                 addError("Internal parser error: Unexpected state parsing value.");
                 return false;
            }

            // After successfully parsing a value
            expected = EXPECT_OPERATOR;
            unaryMinusPossible = false; // Cannot have unary minus right after a value

        }
        // --- Try parsing an operator ---
        else if (String("+-*/%").indexOf(c) != -1) {
             if (expected != EXPECT_OPERATOR) {
                 addError("Syntax error in expression: Unexpected operator '" + String(c) + "'. Expected value.");
                 return false;
             }
             tokens.push_back(ParamValue(String(c), ParamValue::TYPE_OPERATOR));
             expected = EXPECT_VALUE;
             unaryMinusPossible = true; // Allow unary minus after an operator
        }
        // --- Invalid character ---
        else {
             addError("Invalid character '" + String(c) + "' in expression.");
             return false;
        }
    } // End for loop

    // Final checks after loop
    if (tokens.empty()) {
        // Allow empty expression only if the input string itself was empty or whitespace only
        // Create a temporary copy to call trim() on, as trim() modifies the string
        String tempExpression = expressionString;
        tempExpression.trim();
        if (tempExpression.length() > 0) {
            addError("Empty expression parsed from non-empty input.");
            return false;
        }
        // If input was empty/whitespace, return true with empty tokens (e.g., for VAR $x;)
        return true;
    }

    // Expression must end with a value, meaning the final expectation should be for an operator.
    if (expected == EXPECT_VALUE) {
        addError("Syntax error: Expression cannot end with an operator.");
        return false;
    }

    return true;
}


// Parses a condition string (e.g., "$COUNTER > 10", "$X % 2 == 0") into tokens.
// Reuses expression parser logic but might need adjustments for specific condition syntax like modulo.
bool MicroPatternsParser::parseCondition(const String& conditionString, std::vector<ParamValue>& tokens) {
    tokens.clear();
    // For now, treat conditions like expressions. Runtime will interpret operators differently.
    // This handles "value op value" and "$var % literal op value" if parsed correctly.
    // We need to ensure comparison operators are tokenized.

    String currentToken;
    enum State { NONE, NUMBER, VARIABLE } state = NONE;
    bool expectValue = true;

    for (int i = 0; i < conditionString.length(); ++i) {
        char c = conditionString[i];
        char next_c = (i + 1 < conditionString.length()) ? conditionString[i+1] : '\0';

        if (isspace(c)) {
            if (currentToken.length() > 0) {
                ParamValue val = parseValue(currentToken);
                 // Allow values or operators here, let runtime validate structure
                 if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
                 tokens.push_back(val);
                 currentToken = "";
                 state = NONE;
            }
            continue; // Skip whitespace
        }

        // Check for multi-char operators first (==, !=, <=, >=)
        String op = "";
        if (String("=!<>").indexOf(c) != -1) {
            if (next_c == '=') {
                op += c;
                op += next_c;
            } else if (String("=<>").indexOf(c) != -1) { // Single char comparison or equals
                 // Check if it's assignment '=' which is invalid here
                 if (c == '=') {
                     addError("Invalid operator '=' in condition. Use '==' for comparison.");
                     return false;
                 }
                 op += c;
            }
            // Also check for single '!' which is invalid alone
            else if (c == '!') {
                 addError("Invalid operator '!' in condition. Use '!=' for not equals.");
                 return false;
            }
        }
        // Check for arithmetic operators (+-*/%) allowed in conditions (e.g., modulo)
        else if (String("+-*/%").indexOf(c) != -1) {
             op += c;
        }


        if (op.length() > 0) {
            // Process preceding token if any
            if (currentToken.length() > 0) {
                ParamValue val = parseValue(currentToken);
                if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
                tokens.push_back(val);
                currentToken = "";
                state = NONE;
            }
            // Add operator token
            tokens.push_back(ParamValue(op, ParamValue::TYPE_OPERATOR));
            if (op.length() == 2) i++; // Skip the second char of the operator (e.g., '=')
            state = NONE;
        } else if (c == '$') {
             if (currentToken.length() > 0) { addError("Syntax error near '$'."); return false; }
             state = VARIABLE;
             currentToken += c;
        } else if (isdigit(c) || (c == '-' && state == NONE && (tokens.empty() || tokens.back().type == ParamValue::TYPE_OPERATOR))) {
             // Allow '-' only at start or after operator
             if (state == VARIABLE) { currentToken += c; } // Append digit to variable name (e.g. $var1) - check if valid?
             else {
                 if (state != NUMBER && currentToken.length() > 0) { addError("Syntax error near digit."); return false; }
                 state = NUMBER;
                 currentToken += c;
             }
        } else if (isalpha(c) || c == '_') {
             if (state == VARIABLE) { currentToken += c; }
             else { addError("Invalid character '" + String(c) + "' in condition."); return false; }
        } else {
             addError("Invalid character '" + String(c) + "' in condition.");
             return false;
        }
    } // End for loop

    // Add the last token
    if (currentToken.length() > 0) {
        ParamValue val = parseValue(currentToken);
        if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
        tokens.push_back(val);
    }

    if (tokens.empty()) {
        addError("Empty condition.");
        return false;
    }
    // Basic validation: Needs at least value op value (3 tokens)
    // More complex validation (like ensuring comparison operator exists) could be added.
    if (tokens.size() < 3) {
         addError("Invalid condition structure. Expected 'value operator value' or '$var % literal op value'.");
         return false;
    }


    return true;
}