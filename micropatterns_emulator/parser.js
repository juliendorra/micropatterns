// Custom Error for Parsing Issues
class ParseError extends Error {
    constructor(message, lineNumber) {
        super(`Parse Error (Line ${lineNumber}): ${message}`);
        this.name = "ParseError";
        this.lineNumber = lineNumber;
    }
}

class MicroPatternsParser {
    constructor() {
        this.reset();
    }

    reset() {
        this.errors = [];
        this.lineNumber = 0;
        this.assets = { patterns: {}, icons: {} };
        this.variables = new Set(); // Keep track of declared VARs
        this.commandStack = []; // For handling nested REPEAT/IF blocks
    }

    parse(scriptText) {
        this.reset();
        const lines = scriptText.split('\n');
        const topLevelCommands = [];

        for (const rawLine of lines) {
            this.lineNumber++;
            try {
                const command = this._parseLine(rawLine);
                if (command) {
                    // Add command to the correct list (top-level or nested block)
                    if (this.commandStack.length > 0) {
                        const currentBlock = this.commandStack[this.commandStack.length - 1];
                        if (currentBlock.type === 'REPEAT') {
                            currentBlock.commands.push(command);
                        } else if (currentBlock.type === 'IF') {
                            if (currentBlock.processingElse) {
                                if (!currentBlock.elseCommands) currentBlock.elseCommands = [];
                                currentBlock.elseCommands.push(command);
                            } else {
                                currentBlock.thenCommands.push(command);
                            }
                        }
                        // Don't add block start/end commands to their own lists
                    } else {
                        topLevelCommands.push(command);
                    }
                }
            } catch (e) {
                if (e instanceof ParseError) {
                    this.errors.push({ line: e.lineNumber, message: e.message });
                } else {
                    // Catch unexpected errors during parsing
                    this.errors.push({ line: this.lineNumber, message: `Unexpected Parser Error: ${e.message}` });
                    console.error("Unexpected Parser Error:", e); // Log stack for debugging
                }
                // Optionally stop parsing on first error, or continue to collect all errors
                // For now, continue parsing.
            }
        } // End line loop

        // Check for unclosed blocks after parsing all lines
        if (this.commandStack.length > 0) {
            const openBlock = this.commandStack[this.commandStack.length - 1];
            this.errors.push({ line: openBlock.line, message: `Parse Error: Unclosed ${openBlock.type} block started on line ${openBlock.line}.` });
        }

        return {
            commands: topLevelCommands,
            assets: this.assets,
            variables: this.variables,
            errors: this.errors
        };
    }

    // --- Private Helper Methods ---

    _parseLine(rawLine) {
        const line = rawLine.trim();
        if (line === '' || line.startsWith('#')) {
            return null; // Skip empty lines and comments
        }

        const { commandName, argsString } = this._parseCommandAndArgs(line);
        return this._processCommand(commandName, argsString);
    }

    _parseCommandAndArgs(line) {
        const commandMatch = line.match(/^([a-zA-Z_]+)\s*(.*)/);
        if (!commandMatch) {
            throw new ParseError(`Invalid command format.`, this.lineNumber);
        }
        const commandName = commandMatch[1].toUpperCase();
        const argsString = commandMatch[2].trim();
        return { commandName, argsString };
    }

    // Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into a plain { KEY: value, ... } object
    _parseArguments(argsString) {
        const params = {};
        let remainingArgs = argsString.trim();

        const argRegex = /^([a-zA-Z_]+)\s*=\s*/; // Matches "KEY=" part

        while (remainingArgs.length > 0) {
            const match = remainingArgs.match(argRegex);
            if (!match) {
                throw new ParseError(`Invalid argument format near "${remainingArgs}". Expected KEY=VALUE.`, this.lineNumber);
            }

            const key = match[1].toUpperCase();
            remainingArgs = remainingArgs.substring(match[0].length); // Remove "KEY="

            let value;
            let valueString;

            if (remainingArgs.startsWith('"')) {
                // Quoted string value
                const closingQuoteIndex = remainingArgs.indexOf('"', 1);
                if (closingQuoteIndex === -1) {
                    throw new ParseError(`Unterminated string literal for parameter ${key}.`, this.lineNumber);
                }
                valueString = remainingArgs.substring(0, closingQuoteIndex + 1);
                value = this._parseValue(valueString); // Keep quotes for validation, remove later if needed
                remainingArgs = remainingArgs.substring(valueString.length).trim();
            } else {
                // Unquoted value (number, variable, keyword like SOLID)
                const nextSpaceIndex = remainingArgs.indexOf(' ');
                if (nextSpaceIndex === -1) {
                    valueString = remainingArgs;
                    remainingArgs = '';
                } else {
                    valueString = remainingArgs.substring(0, nextSpaceIndex);
                    remainingArgs = remainingArgs.substring(nextSpaceIndex).trim();
                }
                if (valueString === '') {
                     throw new ParseError(`Missing value for parameter ${key}.`, this.lineNumber);
                }
                value = this._parseValue(valueString);
            }

            if (params.hasOwnProperty(key)) {
                 throw new ParseError(`Duplicate parameter: ${key}.`, this.lineNumber);
            }
            params[key] = value;
        }

        return params; // Returns a plain, mutable object
    }

    // Parses a single value string into its type (number, variable string, literal string)
    _parseValue(valueString) {
        valueString = valueString.trim();
        if (valueString.startsWith('"') && valueString.endsWith('"')) {
            return valueString; // Keep as quoted string for now (validation might need quotes)
        } else if (valueString.startsWith('$')) {
            if (!/^\$[a-zA-Z_][a-zA-Z0-9_]*$/.test(valueString)) {
                throw new ParseError(`Invalid variable format: ${valueString}`, this.lineNumber);
            }
            return valueString; // Variable like "$VAR"
        } else if (/^-?[0-9]+$/.test(valueString)) {
            return parseInt(valueString, 10); // Integer literal
        } else {
            // Could be a keyword like BLACK, WHITE, SOLID, or an expression/condition string
            // Let the specific command validation handle these cases.
            return valueString;
        }
    }

    _processCommand(commandName, argsString) {
        let command = { type: commandName, line: this.lineNumber };

        // --- Handle Block Endings ---
        if (commandName === 'ENDREPEAT') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'REPEAT') {
                throw new ParseError(`Unexpected ENDREPEAT without matching REPEAT.`, this.lineNumber);
            }
            return this.commandStack.pop(); // Return the completed REPEAT command object
        }
        if (commandName === 'ENDIF') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ENDIF without matching IF.`, this.lineNumber);
            }
            const ifCommand = this.commandStack.pop();
            delete ifCommand.processingElse; // Clean up temporary state
            return ifCommand; // Return the completed IF command object
        }
        if (commandName === 'ELSE') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ELSE without matching IF.`, this.lineNumber);
            }
            const currentIf = this.commandStack[this.commandStack.length - 1];
            if (currentIf.processingElse) {
                 throw new ParseError(`Multiple ELSE clauses for the same IF.`, this.lineNumber);
            }
            currentIf.processingElse = true; // Mark that we are now parsing the ELSE block
            return null; // ELSE itself doesn't create a command node, it modifies the IF state
        }

        // --- Handle Regular Commands and Block Starts ---
        command.params = this._parseArguments(argsString);
        this._validateAndProcessCommand(command); // Validate params and potentially modify command

        // --- Handle Block Beginnings ---
        if (command.type === 'REPEAT') {
            command.commands = []; // Initialize list for nested commands
            this.commandStack.push(command);
            return null; // Don't return REPEAT start command immediately
        }
        if (command.type === 'IF') {
            command.thenCommands = [];
            command.elseCommands = null; // Initialize (will be array if ELSE is encountered)
            command.processingElse = false; // Internal parser state
            this.commandStack.push(command);
            return null; // Don't return IF start command immediately
        }

        // If it's a regular command (not a block start/end), return it
        return command;
    }


    _validateAndProcessCommand(command) {
        const p = command.params; // Use the parsed params directly

        switch (command.type) {
            case 'DEFINE':
                // DEFINE needs special handling because subtype is part of args
                // Let's re-parse the args string slightly differently for DEFINE
                const defineMatch = command.rawArgsString.match(/^(PATTERN|ICON)\s+(.*)/i); // Assuming rawArgsString is available
                 if (!defineMatch) {
                     throw new ParseError(`DEFINE must be followed by PATTERN or ICON NAME=...`, this.lineNumber);
                 }
                 command.subType = defineMatch[1].toUpperCase();
                 const defineArgsString = defineMatch[2].trim();
                 command.params = this._parseArguments(defineArgsString); // Re-parse args for DEFINE
                 const dp = command.params; // Use the new params

                this._requireParams(dp, ['NAME', 'WIDTH', 'HEIGHT', 'DATA']);
                const name = this._validateString(dp.NAME, 'NAME');
                const width = this._validateInteger(dp.WIDTH, 'WIDTH', 1, 20);
                const height = this._validateInteger(dp.HEIGHT, 'HEIGHT', 1, 20);
                const dataStr = this._validateString(dp.DATA, 'DATA'); // Get raw data string

                if (dataStr.length !== width * height) {
                    throw new ParseError(`DATA length (${dataStr.length}) does not match WIDTH*HEIGHT (${width * height}) for ${name}`, this.lineNumber);
                }
                if (!/^[01]+$/.test(dataStr)) {
                     throw new ParseError(`DATA string must contain only '0' or '1' for ${name}`, this.lineNumber);
                }

                const assetData = { name, width, height, data: dataStr.split('').map(Number) };

                if (command.subType === 'PATTERN') {
                    if (Object.keys(this.assets.patterns).length >= 8) throw new ParseError(`Maximum number of patterns (8) reached.`, this.lineNumber);
                    if (this.assets.patterns[name]) throw new ParseError(`Pattern "${name}" already defined.`, this.lineNumber);
                    this.assets.patterns[name] = assetData;
                } else { // ICON
                     if (Object.keys(this.assets.icons).length >= 16) throw new ParseError(`Maximum number of icons (16) reached.`, this.lineNumber);
                     if (this.assets.icons[name]) throw new ParseError(`Icon "${name}" already defined.`, this.lineNumber);
                    this.assets.icons[name] = assetData;
                }
                command.type = 'NOOP'; // Definition handled at parse time
                delete command.params; // No params needed at runtime
                delete command.subType;
                break;

            case 'VAR':
                 // VAR NAME=varname syntax is not standard, spec says VAR varname
                 // Let's adjust parsing for VAR specifically
                 const varMatch = command.rawArgsString.match(/^([a-zA-Z_][a-zA-Z0-9_]*)$/);
                 if (!varMatch) {
                     throw new ParseError(`Invalid syntax for VAR. Expected 'VAR variable_name'.`, this.lineNumber);
                 }
                 const varName = varMatch[1]; // Keep original case
                 if (this.variables.has(varName)) {
                     throw new ParseError(`Variable "${varName}" already declared.`, this.lineNumber);
                 }
                 if (this._isEnvVar(varName)) {
                      throw new ParseError(`Cannot declare variable with the same name as an environment variable: ${varName}`, this.lineNumber);
                 }
                 this.variables.add(varName);
                 command.varName = varName; // Store for runtime
                 delete command.params; // No params for VAR
                break;

            case 'LET':
                 // LET var = expression syntax
                 const letMatch = command.rawArgsString.match(/^([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.*)$/);
                 if (!letMatch) {
                     throw new ParseError(`Invalid syntax for LET. Expected 'LET variable = expression'.`, this.lineNumber);
                 }
                 const targetVar = letMatch[1];
                 const expressionString = letMatch[2].trim();

                 if (!this.variables.has(targetVar)) {
                     throw new ParseError(`Cannot assign to undeclared variable: "${targetVar}"`, this.lineNumber);
                 }
                 if (expressionString === '') {
                      throw new ParseError(`Missing expression for LET statement.`, this.lineNumber);
                 }

                 command.targetVar = targetVar;
                 command.expression = this._parseExpression(expressionString); // Parse the expression string
                 delete command.params; // Params not used directly for LET
                break;

            case 'COLOR':
                this._requireParams(p, ['NAME']);
                const colorName = String(p.NAME).toUpperCase(); // Ensure it's a string before upper casing
                if (colorName !== 'BLACK' && colorName !== 'WHITE') {
                    throw new ParseError(`Invalid COLOR NAME: "${p.NAME}". Must be BLACK or WHITE.`, this.lineNumber);
                }
                p.NAME = colorName; // Normalize
                break;

            case 'PATTERN':
                 this._requireParams(p, ['NAME']);
                 const patternName = p.NAME; // Can be "SOLID" or "pattern_name"
                 if (typeof patternName === 'string' && patternName.toUpperCase() === 'SOLID') {
                     p.NAME = 'SOLID'; // Normalize keyword
                 } else {
                     // Must be a quoted string literal for a defined pattern
                     this._validateString(patternName, 'NAME');
                     p.NAME = patternName.substring(1, patternName.length - 1); // Store unquoted name
                 }
                 break;

            case 'PIXEL': this._requireParams(p, ['X', 'Y']); this._validateAllParamsAreValues(p); break;
            case 'LINE': this._requireParams(p, ['X1', 'Y1', 'X2', 'Y2']); this._validateAllParamsAreValues(p); break;
            case 'RECT': this._requireParams(p, ['X', 'Y', 'WIDTH', 'HEIGHT']); this._validateAllParamsAreValues(p); break;
            case 'FILL_RECT': this._requireParams(p, ['X', 'Y', 'WIDTH', 'HEIGHT']); this._validateAllParamsAreValues(p); break;
            case 'CIRCLE': this._requireParams(p, ['X', 'Y', 'RADIUS']); this._validateAllParamsAreValues(p); break;
            case 'FILL_CIRCLE': this._requireParams(p, ['X', 'Y', 'RADIUS']); this._validateAllParamsAreValues(p); break;
            case 'ICON':
                this._requireParams(p, ['NAME', 'X', 'Y']);
                p.NAME = this._validateString(p.NAME, 'NAME'); // Ensure NAME is quoted string, store unquoted
                this._validateAllParamsAreValues(p, ['NAME']); // Validate X, Y are values
                break;

            case 'RESET_TRANSFORMS': this._ensureNoParams(p); break;
            case 'TRANSLATE': this._requireParams(p, ['DX', 'DY']); this._validateAllParamsAreValues(p); break;
            case 'ROTATE': this._requireParams(p, ['DEGREES']); this._validateAllParamsAreValues(p); break;
            case 'SCALE':
                this._requireParams(p, ['FACTOR']);
                this._validateInteger(p.FACTOR, 'FACTOR', 1); // Factor >= 1, allow variables too
                this._validateAllParamsAreValues(p);
                 break;

            case 'REPEAT':
                 // REPEAT COUNT=n TIMES syntax
                 const repeatMatch = command.rawArgsString.match(/^COUNT\s*=\s*(.*?)\s+TIMES$/i);
                 if (!repeatMatch) {
                     throw new ParseError(`Invalid syntax for REPEAT. Expected 'REPEAT COUNT=value TIMES'.`, this.lineNumber);
                 }
                 const countValue = this._parseValue(repeatMatch[1].trim());
                 this._validateInteger(countValue, 'COUNT', 0); // Allow COUNT=0, allow variables
                 command.count = countValue; // Store parsed count value/variable
                 delete command.params; // Params not used directly
                break;

            case 'IF':
                 // IF condition THEN syntax
                 const ifMatch = command.rawArgsString.match(/^(.*?)\s+THEN$/i);
                 if (!ifMatch) {
                     throw new ParseError(`Invalid syntax for IF. Expected 'IF condition THEN'.`, this.lineNumber);
                 }
                 const conditionString = ifMatch[1].trim();
                 if (conditionString === '') {
                      throw new ParseError(`Missing condition for IF statement.`, this.lineNumber);
                 }
                 command.condition = this._parseCondition(conditionString);
                 delete command.params; // Params not used directly
                break;

            // ENDREPEAT, ENDIF, ELSE handled earlier
            case 'NOOP': break; // Already processed (e.g., DEFINE)

            default:
                throw new ParseError(`Unknown command type: ${command.type}`, this.lineNumber);
        }
    }

    _requireParams(params, required) {
        for (const req of required) {
            if (!(req in params)) {
                throw new ParseError(`Missing required parameter: ${req}`, this.lineNumber);
            }
        }
    }
     _ensureNoParams(params) {
        if (Object.keys(params).length > 0) {
            throw new ParseError(`Command does not accept any parameters. Found: ${Object.keys(params).join(', ')}`, this.lineNumber);
        }
    }

    // Validates that a parameter value is a quoted string literal, returns the inner content.
    _validateString(value, paramName) {
        if (typeof value !== 'string' || !value.startsWith('"') || !value.endsWith('"')) {
            throw new ParseError(`Parameter ${paramName} requires a string literal in double quotes (e.g., "name"). Got: ${value}`, this.lineNumber);
        }
        return value.substring(1, value.length - 1); // Return inner value
    }

    // Validates that a parameter value is an integer literal or a variable reference ($VAR).
    // Returns the validated number or the variable string.
    _validateInteger(value, paramName, min = -Infinity, max = Infinity) {
        if (typeof value === 'number') {
            if (!Number.isInteger(value)) {
                 throw new ParseError(`Parameter ${paramName} requires an integer value. Got non-integer number: ${value}`, this.lineNumber);
            }
            if (value < min || value > max) {
                throw new ParseError(`Parameter ${paramName} value ${value} is out of range (${min} to ${max}).`, this.lineNumber);
            }
            return value; // Valid integer literal
        } else if (typeof value === 'string' && value.startsWith('$')) {
            // It's a variable, runtime will check range if needed
            return value;
        } else {
            throw new ParseError(`Parameter ${paramName} requires an integer literal or a variable ($var). Got: ${value}`, this.lineNumber);
        }
    }

     // Validates that all parameters in 'params' are either numbers or variables ($VAR),
     // skipping any keys listed in 'excludeKeys'.
     _validateAllParamsAreValues(params, excludeKeys = []) {
         for (const key in params) {
             if (excludeKeys.includes(key)) continue;
             const value = params[key];
             if (typeof value !== 'number' && !(typeof value === 'string' && value.startsWith('$'))) {
                 throw new ParseError(`Parameter ${key} requires an integer literal or a variable ($var). Got: ${value}`, this.lineNumber);
             }
             // Further validation (like range checks for specific params) can be added here or in the main switch
         }
     }


    _isEnvVar(name) {
        // Check if name (without $) is an environment variable
        const coreName = name.startsWith('$') ? name.substring(1) : name;
        const envVars = ['HOUR', 'MINUTE', 'SECOND', 'COUNTER', 'WIDTH', 'HEIGHT', 'INDEX'];
        return envVars.includes(coreName);
    }

    // Parses simple "value op value" or "$var % lit op value"
    _parseCondition(conditionString) {
        conditionString = conditionString.trim();
        // Check for modulo form first: $var % literal op value
        const moduloMatch = conditionString.match(/^(\$[a-zA-Z_][a-zA-Z0-9_]*)\s*%\s*([0-9]+)\s*(==|!=|<=|>=|<|>)\s*(.*)$/);
        if (moduloMatch) {
            const [, leftVar, literalStr, operator, rightRaw] = moduloMatch;
            const right = this._parseValue(rightRaw.trim());
             if (!this.variables.has(leftVar.substring(1)) && !this._isEnvVar(leftVar)) {
                 throw new ParseError(`Unknown variable in condition: ${leftVar}`, this.lineNumber);
             }
             this._validateValueIsVariableOrNumber(right, rightRaw); // Ensure right side is valid
            return { type: 'modulo', leftVar, literal: parseInt(literalStr, 10), operator, right };
        }

        // Standard form: value op value
        const match = conditionString.match(/^(.*?)\s*(==|!=|<=|>=|<|>)\s*(.*)$/);
        if (!match) {
            throw new ParseError(`Invalid condition format: "${conditionString}". Expected 'value operator value'.`, this.lineNumber);
        }
        const [, leftRaw, operator, rightRaw] = match;
        const left = this._parseValue(leftRaw.trim());
        const right = this._parseValue(rightRaw.trim());

        this._validateValueIsVariableOrNumber(left, leftRaw);
        this._validateValueIsVariableOrNumber(right, rightRaw);

        return { type: 'standard', left, operator, right };
    }

     _validateValueIsVariableOrNumber(value, rawValue) {
         if (typeof value === 'number') return; // OK
         if (typeof value === 'string' && value.startsWith('$')) {
             if (!this.variables.has(value.substring(1)) && !this._isEnvVar(value)) {
                 throw new ParseError(`Unknown variable in condition or expression: ${value}`, this.lineNumber);
             }
             return; // OK
         }
         // If it's neither number nor variable, it's invalid in a condition/expression context
         throw new ParseError(`Invalid value in condition or expression: "${rawValue}". Expected integer or variable starting with $.`, this.lineNumber);
     }


     // Parses expression string like "10 + $VAR * 2" into an executable token list
     _parseExpression(exprString) {
         const tokens = [];
         const regex = /(\$[a-zA-Z_][a-zA-Z0-9_]*)|([-+]?[0-9]+)|([+\-*/%])/g;
         let match;
         let lastIndex = 0;
         let expectedType = 'value'; // Expect a value (num/var) first

         while ((match = regex.exec(exprString)) !== null) {
             if (match.index > lastIndex && exprString.substring(lastIndex, match.index).trim() !== '') {
                 throw new ParseError(`Invalid characters in expression: "${exprString.substring(lastIndex, match.index).trim()}"`, this.lineNumber);
             }

             if (match[1]) { // Variable
                 if (expectedType !== 'value') throw new ParseError(`Unexpected variable: ${match[1]}. Expected operator.`, this.lineNumber);
                 const varName = match[1];
                 this._validateValueIsVariableOrNumber(varName, varName); // Check if declared/env
                 tokens.push({ type: 'var', value: varName });
                 expectedType = 'operator';
             } else if (match[2]) { // Number
                 if (expectedType !== 'value') throw new ParseError(`Unexpected number: ${match[2]}. Expected operator.`, this.lineNumber);
                 tokens.push({ type: 'num', value: parseInt(match[2], 10) });
                 expectedType = 'operator';
             } else if (match[3]) { // Operator
                 if (expectedType !== 'operator') throw new ParseError(`Unexpected operator: ${match[3]}. Expected value.`, this.lineNumber);
                 tokens.push({ type: 'op', value: match[3] });
                 expectedType = 'value';
             }
             lastIndex = regex.lastIndex;
         }

         if (lastIndex < exprString.length && exprString.substring(lastIndex).trim() !== '') {
              throw new ParseError(`Invalid trailing characters in expression: "${exprString.substring(lastIndex).trim()}"`, this.lineNumber);
         }
         if (tokens.length === 0) throw new ParseError(`Empty expression.`, this.lineNumber);
         if (expectedType === 'value') throw new ParseError(`Expression cannot end with an operator.`, this.lineNumber); // Last token must be a value

         return tokens; // Return token list for runtime evaluation
     }

     // Helper to add rawArgsString to command before parsing arguments
     // This is needed for commands like DEFINE, VAR, LET, REPEAT, IF that have non-standard arg syntax
     _parseCommandAndArgs(line) {
        const commandMatch = line.match(/^([a-zA-Z_]+)\s*(.*)/);
        if (!commandMatch) {
            throw new ParseError(`Invalid command format.`, this.lineNumber);
        }
        const commandName = commandMatch[1].toUpperCase();
        const argsString = commandMatch[2].trim();
        // Store raw args string for commands that need special parsing
        this.currentRawArgsString = argsString;
        return { commandName, argsString };
    }

     // Modified _processCommand to pass rawArgsString
     _processCommand(commandName, argsString) {
        let command = { type: commandName, line: this.lineNumber, rawArgsString: argsString }; // Add rawArgsString

        // --- Handle Block Endings ---
        // ... (same as before) ...
         if (commandName === 'ENDREPEAT') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'REPEAT') {
                throw new ParseError(`Unexpected ENDREPEAT without matching REPEAT.`, this.lineNumber);
            }
            return this.commandStack.pop(); // Return the completed REPEAT command object
        }
        if (commandName === 'ENDIF') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ENDIF without matching IF.`, this.lineNumber);
            }
            const ifCommand = this.commandStack.pop();
            delete ifCommand.processingElse; // Clean up temporary state
            return ifCommand; // Return the completed IF command object
        }
        if (commandName === 'ELSE') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ELSE without matching IF.`, this.lineNumber);
            }
            const currentIf = this.commandStack[this.commandStack.length - 1];
            if (currentIf.processingElse) {
                 throw new ParseError(`Multiple ELSE clauses for the same IF.`, this.lineNumber);
            }
            currentIf.processingElse = true; // Mark that we are now parsing the ELSE block
            return null; // ELSE itself doesn't create a command node, it modifies the IF state
        }


        // --- Handle Regular Commands and Block Starts ---
         // Only parse arguments if it's not a command with special syntax handled later
         const specialSyntaxCommands = ['DEFINE', 'VAR', 'LET', 'REPEAT', 'IF'];
         if (!specialSyntaxCommands.includes(commandName)) {
            command.params = this._parseArguments(argsString);
         }

        this._validateAndProcessCommand(command); // Validate params/syntax and potentially modify command

        // Clean up rawArgsString if no longer needed
        delete command.rawArgsString;

        // --- Handle Block Beginnings ---
        // ... (same as before) ...
         if (command.type === 'REPEAT') {
            command.commands = []; // Initialize list for nested commands
            this.commandStack.push(command);
            return null; // Don't return REPEAT start command immediately
        }
        if (command.type === 'IF') {
            command.thenCommands = [];
            command.elseCommands = null; // Initialize (will be array if ELSE is encountered)
            command.processingElse = false; // Internal parser state
            this.commandStack.push(command);
            return null; // Don't return IF start command immediately
        }


        // If it's a regular command (not a block start/end), return it
        return command;
    }
}