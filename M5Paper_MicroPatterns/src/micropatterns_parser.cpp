#include "micropatterns_parser.h"
#include <ctype.h> // For isdigit

MicroPatternsParser::MicroPatternsParser() {
    reset();
}

void MicroPatternsParser::reset() {
    _commands.clear();
    _assets.clear();
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

    // --- Handle specific commands ---
    if (commandNameStr == "DEFINE") {
        if (!parseDefinePattern(argsString)) return false;
        cmd.type = CMD_NOOP; // Handled entirely here
        // Don't add NOOP to command list
        return true;
    } else if (commandNameStr == "VAR") {
         if (!parseVar(argsString)) return false;
         // We need the VAR command at runtime to initialize
         cmd.type = CMD_VAR;
         // varName is set within parseVar and copied to the command added below
         // For simplicity, we'll find the varName again here
         int dollarPos = argsString.indexOf('$');
         int equalsPos = argsString.indexOf('=');
         int endPos = (equalsPos != -1) ? equalsPos : argsString.length();
         if (dollarPos != -1) {
             cmd.varName = argsString.substring(dollarPos + 1, endPos);
             cmd.varName.trim();
             cmd.varName.toUpperCase();
         } else {
             addError("Invalid VAR syntax: Missing '$variable_name'.");
             return false;
         }
         // Simplified: No expression parsing for VAR init yet
    } else if (commandNameStr == "LET") {
        if (!parseLet(argsString)) return false;
        cmd.type = CMD_LET;
        // Extract target var and simplified value
        int equalsPos = argsString.indexOf('=');
        if (equalsPos != -1) {
            String targetPart = argsString.substring(0, equalsPos);
            String valuePart = argsString.substring(equalsPos + 1);
            targetPart.trim();
            valuePart.trim();
            if (targetPart.startsWith("$")) {
                cmd.letTargetVar = targetPart.substring(1);
                cmd.letTargetVar.toUpperCase();
                // Parse the value part and store it in the params map
                cmd.params["VALUE"] = parseValue(valuePart);
            } else {
                 addError("Invalid LET syntax: Target variable must start with '$'.");
                 return false;
            }
        } else {
            addError("Invalid LET syntax: Missing '='.");
            return false;
        }
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

    // Parse parameters for commands that use them
    if (cmd.type != CMD_RESET_TRANSFORMS && cmd.type != CMD_VAR && cmd.type != CMD_LET && cmd.type != CMD_NOOP) {
        if (!parseParams(argsString, cmd.params)) {
            // Error already added in parseParams
            return false;
        }
    }

    _commands.push_back(cmd);
    return true;
}

bool MicroPatternsParser::parseDefinePattern(const String& argsString) {
    String remainingArgs = argsString;
    remainingArgs.trim();

    // Create a copy for the case-insensitive check
    String upperArgs = remainingArgs;
    upperArgs.toUpperCase(); // Modify the copy

    if (!upperArgs.startsWith("PATTERN ")) {
        addError("DEFINE must be followed by PATTERN.");
        return false;
    }
    remainingArgs = remainingArgs.substring(8); // Length of "PATTERN "
    remainingArgs.trim();

    std::map<String, ParamValue> params;
    if (!parseParams(remainingArgs, params)) {
        return false; // Error already added
    }

    // Validate required params
    if (params.find("NAME") == params.end() || params["NAME"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires NAME=\"...\" parameter.");
        return false;
    }
    if (params.find("WIDTH") == params.end() || params["WIDTH"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires WIDTH=... parameter.");
        return false;
    }
    if (params.find("HEIGHT") == params.end() || params["HEIGHT"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires HEIGHT=... parameter.");
        return false;
    }
    if (params.find("DATA") == params.end() || params["DATA"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires DATA=\"...\" parameter.");
        return false;
    }

    MicroPatternsAsset asset;
    asset.name = params["NAME"].stringValue; // Keep original case for potential display, use uppercase for key
    asset.name.toUpperCase();
    asset.width = params["WIDTH"].intValue;
    asset.height = params["HEIGHT"].intValue;
    String dataStr = params["DATA"].stringValue;

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
        addError("Pattern '" + params["NAME"].stringValue + "' already defined.");
        return false;
    }

    _assets[asset.name] = asset;
    return true;
}

bool MicroPatternsParser::parseVar(const String& argsString) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();

    if (!trimmedArgs.startsWith("$")) {
        addError("VAR requires a variable name starting with '$'.");
        return false;
    }

    // Simplified: No expression parsing yet. Just register the variable name.
    int spacePos = trimmedArgs.indexOf(' ');
    int equalsPos = trimmedArgs.indexOf('=');
    int endPos = trimmedArgs.length();

    if (equalsPos != -1 && (spacePos == -1 || equalsPos < spacePos)) {
        endPos = equalsPos; // End name at '=' if present
    } else if (spacePos != -1) {
         addError("Invalid VAR syntax. Use 'VAR $name' or 'VAR $name = value'.");
         return false; // Don't allow things like "VAR $x 10"
    }


    String varName = trimmedArgs.substring(1, endPos);
    varName.trim();
    varName.toUpperCase();

    if (varName.length() == 0) {
        addError("Invalid variable name in VAR declaration.");
        return false;
    }

    // Check for re-declaration (case-insensitive)
    for(const String& existingVar : _declaredVariables) {
        if (existingVar.equalsIgnoreCase(varName)) {
            addError("Variable '$" + varName + "' already declared.");
            return false;
        }
    }

    // Check against environment variables (case-insensitive)
    String testName = "$" + varName;
    if (testName.equalsIgnoreCase("$HOUR") || testName.equalsIgnoreCase("$MINUTE") ||
        testName.equalsIgnoreCase("$SECOND") || testName.equalsIgnoreCase("$COUNTER") ||
        testName.equalsIgnoreCase("$WIDTH") || testName.equalsIgnoreCase("$HEIGHT") ||
        testName.equalsIgnoreCase("$INDEX")) {
        addError("Cannot declare variable with the same name as an environment variable: $" + varName);
        return false;
    }


    _declaredVariables.push_back(varName);
    // Note: Initial value parsing/handling would go here. Defaulting to 0 at runtime for now.
    return true;
}

bool MicroPatternsParser::parseLet(const String& argsString) {
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

     String targetVarName = targetVarStr.substring(1);
     targetVarName.toUpperCase();

     // Check if variable was declared
     bool declared = false;
     for(const String& existingVar : _declaredVariables) {
         if (existingVar.equalsIgnoreCase(targetVarName)) {
             declared = true;
             break;
         }
     }
     if (!declared) {
         addError("Cannot assign to undeclared variable: $" + targetVarName);
         return false;
     }

     // Simplified: No expression parsing yet. Assume integer literal.
     // Real implementation would parse expressionStr here.
     return true;
}


// Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into the params map
bool MicroPatternsParser::parseParams(const String& argsString, std::map<String, ParamValue>& params) {
    int currentPos = 0;
    String remainingArgs = argsString;
    remainingArgs.trim();

    while (currentPos < remainingArgs.length()) {
        int equalsPos = remainingArgs.indexOf('=', currentPos);
        if (equalsPos == -1) {
            addError("Invalid parameter format near '" + remainingArgs.substring(currentPos) + "'. Expected KEY=VALUE.");
            return false;
        }

        String key = remainingArgs.substring(currentPos, equalsPos);
        key.trim();
        key.toUpperCase(); // Parameter names are case-insensitive

        if (key.length() == 0) {
             addError("Empty parameter name found.");
             return false;
        }

        int valueStart = equalsPos + 1;
        while (valueStart < remainingArgs.length() && remainingArgs[valueStart] == ' ') {
            valueStart++; // Skip spaces after '='
        }

        if (valueStart >= remainingArgs.length()) {
            addError("Missing value for parameter " + key + ".");
            return false;
        }

        String valueString;
        int valueEnd;

        if (remainingArgs[valueStart] == '"') {
            // Quoted string value
            valueStart++; // Skip opening quote
            valueEnd = valueStart;
            bool foundEndQuote = false;
            while (valueEnd < remainingArgs.length()) {
                if (remainingArgs[valueEnd] == '"') {
                    // Basic handling: doesn't support escaped quotes within
                    foundEndQuote = true;
                    break;
                }
                valueEnd++;
            }

            if (!foundEndQuote) {
                addError("Unterminated string literal for parameter " + key + ".");
                return false;
            }
            valueString = remainingArgs.substring(valueStart, valueEnd);
            params[key] = ParamValue(valueString, false); // Store as string
            currentPos = valueEnd + 1; // Move past closing quote
        } else {
            // Unquoted value (number, variable, keyword)
            valueEnd = valueStart;
            while (valueEnd < remainingArgs.length()) {
                // Value ends at the next space *unless* it's part of the next parameter key=
                if (remainingArgs[valueEnd] == ' ') {
                    // Look ahead to see if the next part looks like KEY=
                    int nextEquals = remainingArgs.indexOf('=', valueEnd);
                    int nextSpaceAfterVal = remainingArgs.indexOf(' ', valueEnd + 1); // Find space *after* current potential value end

                    bool looksLikeNextParam = false;
                    if(nextEquals != -1) {
                        // Check if the equals sign appears before the next space (or if there's no next space)
                        if(nextSpaceAfterVal == -1 || nextEquals < nextSpaceAfterVal) {
                             // Check if the part before '=' is a valid key (simple check: no spaces)
                             String potentialNextKey = remainingArgs.substring(valueEnd, nextEquals);
                             potentialNextKey.trim();
                             if(potentialNextKey.indexOf(' ') == -1 && potentialNextKey.length() > 0) {
                                 looksLikeNextParam = true;
                             }
                        }
                    }

                    if (looksLikeNextParam) {
                        break; // End current value here
                    }
                }
                 valueEnd++;
            }
            valueString = remainingArgs.substring(valueStart, valueEnd);
            valueString.trim(); // Trim the extracted value itself
            if (valueString.length() == 0) {
                 addError("Missing value for parameter " + key + ".");
                 return false;
            }
            params[key] = parseValue(valueString);
            currentPos = valueEnd; // Move past the parsed value
        }

        // Skip whitespace before next potential parameter
        while (currentPos < remainingArgs.length() && remainingArgs[currentPos] == ' ') {
            currentPos++;
        }
    }

    return true;
}

// Parses a single unquoted value string
ParamValue MicroPatternsParser::parseValue(const String& valueString) {
    if (valueString.startsWith("$")) {
        // Variable reference (case-insensitive name stored internally)
        // Basic validation
        if (valueString.length() <= 1) {
             addError("Invalid variable format: '$' must be followed by a name.");
             return ParamValue(0); // Return dummy value on error
        }
        // Store the variable name including '$', runtime will resolve
        // Convert variable reference to uppercase for case-insensitive handling at runtime
        String upperVarName = valueString; // Create a copy
        upperVarName.toUpperCase();       // Modify the copy
        return ParamValue(upperVarName, true); // Use the modified copy
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