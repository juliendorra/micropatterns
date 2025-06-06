// Custom Error for Parsing Issues
export class ParseError extends Error {
    constructor(message, lineNumber) {
        super(`Parse Error (Line ${lineNumber}): ${message}`);
        this.name = "ParseError";
        this.lineNumber = lineNumber;
    }
}

export class MicroPatternsParser {
    constructor() {
        this.reset();
    }

    reset() {
        this.errors = [];
        this.lineNumber = 0;
        // Unified asset storage - all defined items are stored here
        this.assets = { assets: {} };
        this.variables = new Set(); // Keep track of declared VARs (stores uppercase names)
        this.commandStack = []; // For handling nested REPEAT/IF blocks
    }

    parse(scriptText) {
        this.reset();
        const lines = scriptText.split('\n');
        const topLevelCommands = [];

        for (const rawLine of lines) {
            this.lineNumber++;
            try {
                const line = rawLine.trim();
                if (line === '' || line.startsWith('#')) {
                    continue; // Skip empty lines and comments
                }

                const { commandName, argsString } = this._parseCommandAndArgs(line);
                const command = this._processCommand(commandName, argsString);

                if (command) { // Only process valid, non-null commands
                    // Add command to the correct list (top-level or nested block)
                    if (this.commandStack.length > 0) {
                        const currentBlock = this.commandStack[this.commandStack.length - 1];
                        // Add command to the currently open block
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
                        // Block start/end commands themselves are handled by _processCommand returning null or the completed block
                    } else {
                        // Add to top level if no block is open
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
                // Continue parsing to collect all errors.
            }
        } // End line loop

        // Check for unclosed blocks after parsing all lines
        if (this.commandStack.length > 0) {
            const openBlock = this.commandStack[this.commandStack.length - 1];
            // Use the line number where the block started for the error message
            this.errors.push({ line: openBlock.line, message: `Parse Error: Unclosed ${openBlock.type} block started on line ${openBlock.line}. Expected END${openBlock.type}.` });
        }

        return {
            commands: topLevelCommands,
            assets: this.assets, // Contains assets.assets
            variables: this.variables, // Set of uppercase variable names
            errors: this.errors
        };
    }

    // --- Private Helper Methods ---

    // Parses the command name (uppercase) and the raw argument string
    _parseCommandAndArgs(line) {
        const commandMatch = line.match(/^([a-zA-Z_]+)\s*(.*)/);
        if (!commandMatch) {
            throw new ParseError(`Invalid command format.`, this.lineNumber);
        }
        const commandName = commandMatch[1].toUpperCase();
        const argsString = commandMatch[2].trim(); // Raw arguments string
        return { commandName, argsString };
    }

    // Processes a command, handles block start/end, validation, and returns the command object or null
    _processCommand(commandName, argsString) {
        let command = { type: commandName, line: this.lineNumber, rawArgsString: argsString }; // Attach raw string immediately

        // --- Handle Block Endings ---
        if (commandName === 'ENDREPEAT') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'REPEAT') {
                throw new ParseError(`Unexpected ENDREPEAT without matching REPEAT.`, this.lineNumber);
            }
            const completedBlock = this.commandStack.pop();
            delete completedBlock.rawArgsString; // Clean up raw string from block command
            return completedBlock; // Return the completed REPEAT command object
        }
        if (commandName === 'ENDIF') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ENDIF without matching IF.`, this.lineNumber);
            }
            const completedBlock = this.commandStack.pop();
            delete completedBlock.processingElse; // Clean up temporary state
            delete completedBlock.rawArgsString; // Clean up raw string from block command
            return completedBlock; // Return the completed IF command object
        }
        if (commandName === 'ELSE') {
            if (this.commandStack.length === 0 || this.commandStack[this.commandStack.length - 1].type !== 'IF') {
                throw new ParseError(`Unexpected ELSE without matching IF.`, this.lineNumber);
            }
            const currentIf = this.commandStack[this.commandStack.length - 1];
            if (!currentIf.thenCommands) { // Should have been initialized
                 throw new ParseError(`Internal Error: IF block missing THEN commands before ELSE.`, this.lineNumber);
            }
            if (currentIf.elseCommands !== null) { // Check if ELSE block already started or exists
                 throw new ParseError(`Multiple ELSE clauses for the same IF statement (started on line ${currentIf.line}).`, this.lineNumber);
            }
            currentIf.processingElse = true; // Mark that we are now parsing the ELSE block
            currentIf.elseCommands = []; // Initialize the ELSE command list
            return null; // ELSE itself doesn't create a command node, it modifies the IF state on the stack
        }

        // --- Handle Regular Commands and Block Starts ---
        // Validate/process, which will parse args internally as needed
        this._validateAndProcessCommand(command); // This might change command.type to NOOP

        // --- Handle Block Beginnings ---
        if (command.type === 'REPEAT') {
            command.commands = []; // Initialize list for nested commands
            this.commandStack.push(command);
            return null; // Don't return REPEAT start command immediately, wait for ENDREPEAT
        }
        if (command.type === 'IF') {
            command.thenCommands = [];
            command.elseCommands = null; // Initialize (will be array if ELSE is encountered)
            command.processingElse = false; // Internal parser state for ELSE logic
            this.commandStack.push(command);
            return null; // Don't return IF start command immediately, wait for ENDIF
        }

        // If it's a regular command (not a block start/end/noop), return it
        if (command.type !== 'NOOP') {
             return command;
        } else {
             return null; // Don't return NOOP commands (like DEFINE PATTERN) to be added to command lists
        }
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

            // Store both uppercase and original case for parameters
            // This helps with backward compatibility for scripts using lowercase params
            const originalKey = match[1];
            const key = originalKey.toUpperCase(); // Parameter names are case-insensitive
            remainingArgs = remainingArgs.substring(match[0].length); // Remove "KEY="

            let value;
            let valueString;

            if (remainingArgs.startsWith('"')) {
                // Quoted string value
                let closingQuoteIndex = -1;
                let currentPos = 1;
                while(currentPos < remainingArgs.length) {
                    if (remainingArgs[currentPos] === '"') {
                        closingQuoteIndex = currentPos;
                        break;
                    }
                    // Allow escaped quotes inside (optional, but good practice)
                    if (remainingArgs[currentPos] === '\\' && currentPos + 1 < remainingArgs.length) {
                        currentPos++; // Skip the escaped character
                    }
                    currentPos++;
                }

                if (closingQuoteIndex === -1) {
                    throw new ParseError(`Unterminated string literal for parameter ${key}.`, this.lineNumber);
                }
                valueString = remainingArgs.substring(0, closingQuoteIndex + 1);
                // Keep quotes for validation, remove later if needed by specific command
                value = valueString;
                remainingArgs = remainingArgs.substring(valueString.length).trim();
            } else {
                // Unquoted value (number, variable, keyword like SOLID)
                // Value ends at the next space that is not preceded by '=' for the next key
                let nextSpaceIndex = -1;
                let tempRemaining = remainingArgs;
                let searchIndex = 0;
                while(searchIndex < tempRemaining.length) {
                    let spaceIndex = tempRemaining.indexOf(' ', searchIndex);
                    if (spaceIndex === -1) { // No more spaces
                        nextSpaceIndex = -1;
                        break;
                    }
                    // Look ahead: is the next non-space char part of a KEY=?
                    let lookAhead = tempRemaining.substring(spaceIndex).trim();
                    if (argRegex.test(lookAhead)) {
                        nextSpaceIndex = spaceIndex; // Found the end of the current value
                        break;
                    } else {
                        // This space is part of the current value (e.g., in a condition string)
                        // Continue searching after this space
                        searchIndex = spaceIndex + 1;
                    }
                }


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

        return params; // Returns a plain, mutable object {UPPER_KEY: value}
    }

    // Parses a single value string into its type (number, variable string, literal string, keyword)
    _parseValue(valueString) {
        // Handle empty strings explicitly
        if (!valueString || valueString.trim() === '') {
            throw new ParseError(`Empty value is not allowed`, this.lineNumber);
        }
        
        valueString = valueString.trim();
        
        if (valueString.startsWith('"') && valueString.endsWith('"')) {
            // Return the raw quoted string, validation might need it
            return valueString;
        } else if (valueString.startsWith('$')) {
            // Variable reference (case-insensitive name stored internally)
            if (!/^\$[a-zA-Z_][a-zA-Z0-9_]*$/.test(valueString)) {
                throw new ParseError(`Invalid variable format: ${valueString}`, this.lineNumber);
            }
            // Convert variable reference to uppercase for case-insensitive handling
            // Format: $VARNAME (uppercase after the $ sign)
            return valueString.toUpperCase();
        } else if (/^-?[0-9]+$/.test(valueString)) {
            // Integer literal
            return parseInt(valueString, 10);
        } else if (valueString === "0") {
            // Special case for "0" to ensure it's treated as a number
            return 0;
        } else {
            // Could be a keyword (like BLACK, WHITE, SOLID) or part of an expression/condition
            // Return the raw string, let specific command validation handle it.
            // Keywords are typically handled case-insensitively by comparing their uppercase version.
            return valueString;
        }
    }

    // Validates command syntax, parses arguments/expressions, checks types, stores assets/variables.
    // Modifies the command object in place (e.g., adds parsed params, expression tokens, sets type to NOOP).
    _validateAndProcessCommand(command) {
        // Commands that need special argument/syntax handling
        const specialSyntaxCommands = ['DEFINE', 'VAR', 'LET', 'REPEAT', 'IF'];
        let p = {}; // Default empty params for standard commands

        // Parse arguments only for standard commands (not DEFINE, VAR, LET, REPEAT, IF)
        if (!specialSyntaxCommands.includes(command.type)) {
            try {
                command.params = this._parseArguments(command.rawArgsString);
                p = command.params; // Use parsed params for validation
            } catch (e) {
                // If arg parsing fails for standard commands, rethrow
                throw e;
            }
        }
        // For special commands, parsing happens inside the switch case using command.rawArgsString

        switch (command.type) {
            case 'DEFINE':
                // Only allow DEFINE PATTERN
                const defineMatch = command.rawArgsString.match(/^PATTERN\s+(.*)/i);
                 if (!defineMatch) {
                     // Check if they tried DEFINE ICON and give specific error
                     if (/^ICON\s+/i.test(command.rawArgsString)) {
                         throw new ParseError(`DEFINE ICON is no longer supported. Use DEFINE PATTERN for all assets.`, command.line);
                     }
                     throw new ParseError(`DEFINE must be followed by PATTERN NAME=...`, command.line);
                 }
                 const defineArgsString = defineMatch[1].trim();
                 // Parse the specific args for DEFINE PATTERN
                 const dp = this._parseArguments(defineArgsString);

                this._requireParams(dp, ['NAME', 'WIDTH', 'HEIGHT', 'DATA']);
                const nameRaw = this._validateString(dp.NAME, 'NAME'); // Get unquoted original case name
                const name = nameRaw.toUpperCase(); // Use uppercase for storage/lookup key
                const width = this._validateInteger(dp.WIDTH, 'WIDTH', 1, 20);
                const height = this._validateInteger(dp.HEIGHT, 'HEIGHT', 1, 20);
                let dataStr = this._validateString(dp.DATA, 'DATA'); // Get unquoted data string
                const expectedLength = width * height;

                if (dataStr.length !== expectedLength) {
                    const mismatchType = dataStr.length < expectedLength ? 'short' : 'long';
                    const actionTaken = mismatchType === 'short' ? 'padded with 0s' : 'truncated';
                    console.warn(`Parser Warning (Line ${command.line}): DATA length (${dataStr.length}) for PATTERN "${nameRaw}" does not match WIDTH*HEIGHT (${expectedLength}). Data will be ${actionTaken}.`);

                    if (mismatchType === 'short') {
                        dataStr = dataStr.padEnd(expectedLength, '0'); // Pad with '0' (white)
                    } else { // long
                        dataStr = dataStr.substring(0, expectedLength); // Truncate
                    }
                }

                if (!/^[01]+$/.test(dataStr)) {
                     // Still error if data contains invalid characters after potential padding/truncation
                     throw new ParseError(`DATA string must contain only '0' or '1' for pattern "${nameRaw}"`, command.line);
                }

                // Store asset data using uppercase name as key in the unified storage
                // Also store originalName for display purposes
                const assetData = {
                    name: name, // Uppercase key for lookup
                    originalName: nameRaw, // Original case for display
                    width,
                    height,
                    data: dataStr.split('').map(Number) // Use the potentially adjusted dataStr
                };

                // Unified asset storage and limit (e.g., 16 total)
                if (Object.keys(this.assets.assets).length >= 16) throw new ParseError(`Maximum number of defined patterns (16) reached.`, command.line);
                if (this.assets.assets[name]) throw new ParseError(`Pattern "${nameRaw}" (or equivalent case) already defined.`, command.line);
                this.assets.assets[name] = assetData;

                command.type = 'NOOP'; // Definition handled at parse time
                break;

            case 'VAR':
                 // Syntax: VAR $varName [= expression]
                 const varMatch = command.rawArgsString.match(/^\$([a-zA-Z_][a-zA-Z0-9_]*)\s*(=)?\s*(.*)?$/);
                 if (!varMatch) {
                     throw new ParseError(`Invalid syntax for VAR. Expected 'VAR $variable_name [= expression]'.`, command.line);
                 }
                 const varNameRaw = varMatch[1]; // Capture name *without* $
                 const hasAssignment = varMatch[2] === '=';
                 const expressionString = (varMatch[3] || '').trim();
                 const varName = varNameRaw.toUpperCase(); // Use uppercase bare name for storage/lookup
                 const varRefRaw = `$${varNameRaw}`; // Original reference with $ for errors

                 if (this.variables.has(varName)) {
                     throw new ParseError(`Variable "${varRefRaw}" (or equivalent case) already declared.`, command.line);
                 }
                 if (this._isEnvVar(varName)) { // Check uppercase bare name against env vars
                      throw new ParseError(`Cannot declare variable with the same name as an environment variable: ${varRefRaw}`, command.line);
                 }

                 // Add variable to declared set *before* parsing expression (allows self-reference if needed, though maybe bad practice)
                 this.variables.add(varName); // Add uppercase bare name to set
                 command.varName = varName; // Store uppercase bare name for runtime initialization

                 if (hasAssignment) {
                     if (expressionString === '') {
                         throw new ParseError(`Missing expression after '=' for VAR declaration.`, command.line);
                     }
                     // Parse the expression string into tokens
                     command.initialExpression = this._parseExpression(expressionString);
                 } else {
                     if (expressionString !== '') {
                         // Case like "VAR $foo 123" - invalid
                         throw new ParseError(`Invalid syntax for VAR. Found unexpected content after variable name. Use '=' for initialization.`, command.line);
                     }
                     // No assignment, will default to 0 at runtime
                     command.initialExpression = null;
                 }
                 command.type = 'VAR'; // Keep type for runtime init
                 break;

            case 'LET':
                 // Require '$' prefix for target variable name
                 const letMatch = command.rawArgsString.match(/^\$([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.*)$/);
                 if (!letMatch) {
                     throw new ParseError(`Invalid syntax for LET. Expected 'LET $variable = expression'.`, command.line);
                 }
                 const targetVarRaw = letMatch[1]; // Capture name *without* $
                 const targetVar = targetVarRaw.toUpperCase(); // Use uppercase bare name for lookup/storage
                 const targetVarRefRaw = `$${targetVarRaw}`; // Original reference with $ for errors
                 const letExpressionString = letMatch[2].trim(); // Renamed variable

                 if (!this.variables.has(targetVar)) { // Check if uppercase bare variable is declared
                     throw new ParseError(`Cannot assign to undeclared variable: "${targetVarRefRaw}"`, command.line);
                 }
                 if (letExpressionString === '') {
                      throw new ParseError(`Missing expression for LET statement.`, command.line);
                 }

                 command.targetVar = targetVar; // Store uppercase bare target variable name
                 command.expression = this._parseExpression(letExpressionString); // Use renamed variable
                 command.type = 'LET'; // Keep type for runtime
                 break;

            case 'COLOR':
                this._requireParams(p, ['NAME']);
                const colorName = String(p.NAME).toUpperCase(); // Ensure it's a string before upper casing
                if (colorName !== 'BLACK' && colorName !== 'WHITE') {
                    throw new ParseError(`Invalid COLOR NAME: "${p.NAME}". Must be BLACK or WHITE.`, command.line);
                }
                p.NAME = colorName; // Normalize to uppercase keyword
                break;

            case 'FILL': // Replaces old PATTERN command
                 this._requireParams(p, ['NAME']);
                 const fillNameValue = p.NAME; // Can be "SOLID" or "pattern_name" (quoted string or keyword)
                 if (typeof fillNameValue === 'string' && fillNameValue.toUpperCase() === 'SOLID') {
                     p.NAME = 'SOLID'; // Normalize keyword
                 } else {
                     // Must be a quoted string literal for a defined pattern
                     const unquotedName = this._validateString(fillNameValue, 'NAME'); // Returns unquoted string
                     p.NAME = unquotedName.toUpperCase(); // Store unquoted, uppercase name for runtime lookup
                     // Runtime will check if this uppercase name exists in assets.assets
                 }
                 break;

            // --- Drawing Commands (Standard Args) ---
            case 'PIXEL': this._requireParams(p, ['X', 'Y']); this._validateAllParamsAreValues(p); break;
            case 'FILL_PIXEL': this._requireParams(p, ['X', 'Y']); this._validateAllParamsAreValues(p); break; // Added FILL_PIXEL
            case 'LINE': this._requireParams(p, ['X1', 'Y1', 'X2', 'Y2']); this._validateAllParamsAreValues(p); break;
            case 'RECT': this._requireParams(p, ['X', 'Y', 'WIDTH', 'HEIGHT']); this._validateAllParamsAreValues(p); break;
            case 'FILL_RECT': this._requireParams(p, ['X', 'Y', 'WIDTH', 'HEIGHT']); this._validateAllParamsAreValues(p); break;
            case 'CIRCLE': this._requireParams(p, ['X', 'Y', 'RADIUS']); this._validateAllParamsAreValues(p); break;
            case 'FILL_CIRCLE': this._requireParams(p, ['X', 'Y', 'RADIUS']); this._validateAllParamsAreValues(p); break;
            case 'DRAW': // Replaces old ICON command
                // p already has uppercase keys from _parseArguments (e.g., NAME, X, Y)
                this._requireParams(p, ['NAME', 'X', 'Y']); // Ensure NAME, X, and Y are present
                const patternNameRawDraw = this._validateString(p.NAME, 'NAME'); // Ensure NAME is quoted string, get unquoted
                p.NAME = patternNameRawDraw.toUpperCase(); // Store unquoted, uppercase name for runtime lookup
                // Validate all params (X, Y) are values (numbers or $vars), excluding the already processed NAME.
                this._validateAllParamsAreValues(p, ['NAME']);
                // Runtime will check if this uppercase name exists in assets.assets
                break;

            // --- Transform Commands (Standard Args) ---
            case 'RESET_TRANSFORMS': this._ensureNoParams(p); break;
            case 'TRANSLATE': this._requireParams(p, ['DX', 'DY']); this._validateAllParamsAreValues(p); break;
            case 'ROTATE': this._requireParams(p, ['DEGREES']); this._validateAllParamsAreValues(p); break;
            case 'SCALE':
                this._requireParams(p, ['FACTOR']);
                // Validate FACTOR is integer >= 1 (or variable)
                this._validateInteger(p.FACTOR, 'FACTOR', 1);
                this._validateAllParamsAreValues(p); // Ensure it's number or var
                 break;

            // --- Control Flow (Special Syntax) ---
            case 'REPEAT':
                 // Updated regex: Remove TIMES. Expects "COUNT=value"
                 const repeatMatch = command.rawArgsString.match(/^COUNT\s*=\s*(.*)$/i);
                 if (!repeatMatch) {
                     throw new ParseError(`Invalid syntax for REPEAT. Expected 'REPEAT COUNT=value'.`, command.line);
                 }
                 const countValueString = repeatMatch[1].trim();
                 if (countValueString === '') {
                     throw new ParseError(`Missing value for REPEAT COUNT.`, command.line);
                 }
                 const countValue = this._parseValue(countValueString);
                 // Validate count is non-negative integer or variable
                 this._validateInteger(countValue, 'COUNT', 0);
                 command.count = countValue; // Store parsed count value/variable
                 command.type = 'REPEAT'; // Keep type for block start logic
                break;

            case 'IF':
                 const ifMatch = command.rawArgsString.match(/^(.*?)\s+THEN$/i);
                 if (!ifMatch) {
                     throw new ParseError(`Invalid syntax for IF. Expected 'IF condition THEN'.`, command.line);
                 }
                 const conditionString = ifMatch[1].trim();
                 if (conditionString === '') {
                      throw new ParseError(`Missing condition for IF statement.`, command.line);
                 }
                 command.condition = this._parseCondition(conditionString); // Parse condition string into object
                 command.type = 'IF'; // Keep type for block start logic
                break;

            // ENDREPEAT, ENDIF, ELSE handled earlier in _processCommand
            case 'NOOP': break; // Already processed (e.g., DEFINE PATTERN sets type to NOOP)

            // --- Deprecated/Removed Commands ---
            case 'PATTERN': // The old state command is now FILL
                throw new ParseError(`Invalid command: PATTERN. Use 'FILL NAME="pattern_name"' or 'FILL NAME=SOLID' to set the fill pattern.`, command.line);
            case 'ICON': // The old drawing command is now DRAW
                throw new ParseError(`Invalid command: ICON. Use 'DRAW NAME="pattern_name" X=x Y=y' to draw a defined pattern.`, command.line);

            default:
                // If it wasn't a special command and didn't match a standard one
                throw new ParseError(`Unknown command type: ${command.type}`, command.line);
        }

        // Clean up rawArgsString after processing
        delete command.rawArgsString;
    }

    // --- Validation Helpers ---

    _requireParams(params, required) {
        // Assumes params object already has its keys in UPPERCASE due to _parseArguments.
        for (const req of required) { // req is expected to be an UPPERCASE string.
            if (!(req in params)) {
                throw new ParseError(`Missing required parameter: ${req}`, this.lineNumber);
            }
        }
    }

     _ensureNoParams(params) {
        if (Object.keys(params).length > 0) {
            throw new ParseError(`Command ${this.type} does not accept any parameters. Found: ${Object.keys(params).join(', ')}`, this.lineNumber);
        }
    }

    // Validates that a parameter value is a quoted string literal, returns the inner content.
    _validateString(value, paramName) {
        if (typeof value !== 'string' || !value.startsWith('"') || !value.endsWith('"')) {
            throw new ParseError(`Parameter ${paramName} requires a string literal in double quotes (e.g., "name"). Got: ${value}`, this.lineNumber);
        }
        if (value.length < 2) { // Empty quotes ""
             throw new ParseError(`Parameter ${paramName} cannot be an empty string "".`, this.lineNumber);
        }
        // Basic unescaping for \" and \\ - can be expanded if needed
        return value.substring(1, value.length - 1)
                    .replace(/\\"/g, '"')
                    .replace(/\\\\/g, '\\');
    }

    // Validates that a parameter value is an integer literal or a variable reference ($VAR).
    // Checks range if it's a literal. Returns the validated number or the variable string.
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
            // It's a variable reference (e.g., $myVar)
            // Runtime will resolve and check range if needed
            this._validateVariableExists(value); // Check if variable is declared/env
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
             if (typeof value === 'number') {
                 // Optional: Add range checks for specific keys like X, Y, WIDTH, HEIGHT?
                 if (!Number.isInteger(value)) {
                     throw new ParseError(`Parameter ${key} requires an integer value. Got non-integer number: ${value}`, this.lineNumber);
                 }
             } else if (typeof value === 'string' && value.startsWith('$')) {
                 this._validateVariableExists(value); // Check if variable is declared/env
             } else {
                 throw new ParseError(`Parameter ${key} requires an integer literal or a variable ($var). Got: ${value}`, this.lineNumber);
             }
         }
     }

    // Checks if a variable name (like "$myVar") corresponds to a declared variable or env variable.
    // Uses uppercase for checks. Throws ParseError if not found.
    _validateVariableExists(varRef) {
        if (typeof varRef !== 'string' || !varRef.startsWith('$')) {
             throw new ParseError(`Internal Error: Expected variable reference ($var), got ${varRef}`, this.lineNumber);
        }
        const varName = varRef.substring(1).toUpperCase(); // Check uppercase version
        if (!this.variables.has(varName) && !this._isEnvVar(varName)) {
            // Use original case in error message for clarity
            throw new ParseError(`Unknown variable: ${varRef}`, this.lineNumber);
        }
    }

    // Checks if an uppercase name is an environment variable.
    _isEnvVar(upperCaseName) {
        const envVars = ['HOUR', 'MINUTE', 'SECOND', 'COUNTER', 'WIDTH', 'HEIGHT', 'INDEX'];
        return envVars.includes(upperCaseName);
    }

    // Parses simple "value op value" or "$var % lit op value" condition strings.
    // Returns a condition object { type, left, operator, right } or { type, leftVar, literal, operator, right }
    _parseCondition(conditionString) {
        conditionString = conditionString.trim();
        // Check for modulo form first: $var % literal op value
        // Regex ensures $var, literal, and operator are captured correctly
        const moduloMatch = conditionString.match(/^(\$[a-zA-Z_][a-zA-Z0-9_]*)\s*%\s*([0-9]+)\s*(==|!=|<=|>=|<|>)\s*(.*)$/);
        if (moduloMatch) {
            const [, leftVarRaw, literalStr, operator, rightRaw] = moduloMatch;
            const leftVar = leftVarRaw.toUpperCase(); // Store uppercase variable
            const literal = parseInt(literalStr, 10);
            const rightTokens = this._parseExpression(rightRaw.trim()); // Parse right-hand side as an expression

            this._validateVariableExists(leftVarRaw); // Validate original variable exists
            if (literal <= 0) {
                 throw new ParseError(`Modulo literal in condition must be positive. Got: ${literalStr}`, this.lineNumber);
            }
            // Validation of rightTokens is handled by _parseExpression.

            return { type: 'modulo', leftVar, literal, operator, right: rightTokens };
        }

        // Standard form: value op value
        // Regex allows variables, numbers on either side
        const match = conditionString.match(/^(.*?)\s*(==|!=|<=|>=|<|>)\s*(.*)$/);
        if (!match) {
            throw new ParseError(`Invalid condition format: "${conditionString}". Expected 'value operator value' or '$var % literal op value'.`, this.lineNumber);
        }
        const [, leftRaw, operator, rightRaw] = match;
        
        // Handle empty operands explicitly
        if (leftRaw.trim() === '') {
            throw new ParseError(`Missing left operand in condition: "${conditionString}"`, this.lineNumber);
        }
        
        if (rightRaw.trim() === '') {
            throw new ParseError(`Missing right operand in condition: "${conditionString}"`, this.lineNumber);
        }
        
        const leftTokens = this._parseExpression(leftRaw.trim());
        const rightTokens = this._parseExpression(rightRaw.trim());

        // Validation of operands is handled by _parseExpression.
        // The structure of leftTokens/rightTokens (e.g. single var/num or complex expression)
        // will be evaluated at runtime.

        return { type: 'standard', left: leftTokens, operator, right: rightTokens };
    }

     // Validates that a parsed value is suitable for conditions/expressions (number or variable).
     // Throws ParseError if not.
     _validateValueIsVariableOrNumber(value, rawValue) {
         // Handle null or undefined explicitly
         if (value === null || value === undefined) {
             throw new ParseError(`Missing or invalid value in condition or expression: "${rawValue}".`, this.lineNumber);
         }
         
         if (typeof value === 'number') {
             if (!Number.isInteger(value)) { // Ensure integer
                 throw new ParseError(`Invalid non-integer number in condition or expression: "${rawValue}".`, this.lineNumber);
             }
             return; // OK - integer value
         }
         
         if (typeof value === 'string' && value.startsWith('$')) {
             this._validateVariableExists(value); // Checks if declared/env
             return; // OK - variable reference
         }
         
         // Special case for "0" as a string - convert to number
         if (rawValue === "0") {
             return; // Allow "0" as a valid literal
         }
         
         // If it's neither integer nor variable, it's invalid
         throw new ParseError(`Invalid value in condition or expression: "${rawValue}". Expected integer or variable starting with $.`, this.lineNumber);
     }


     // Parses expression string like "10 + $VAR * 2" into an executable token list [{type:'num', value:10}, {type:'op', value:'+'}, ...]
     // Stores variable tokens with uppercase names.
     _parseExpression(exprString) {
         const tokens = [];
         // Regex captures variables ($var), numbers (including negative), and operators
         const regex = /(\$[a-zA-Z_][a-zA-Z0-9_]*)|([-+]?[0-9]+)|([+\-*/%])/g;
         let match;
         let lastIndex = 0;
         let expectedType = 'value'; // Expect a value (num/var) first

         while ((match = regex.exec(exprString)) !== null) {
             // Check for invalid characters between tokens
             if (match.index > lastIndex && exprString.substring(lastIndex, match.index).trim() !== '') {
                 throw new ParseError(`Invalid characters in expression: "${exprString.substring(lastIndex, match.index).trim()}"`, this.lineNumber);
             }

             if (match[1]) { // Variable ($var)
                 if (expectedType !== 'value') throw new ParseError(`Unexpected variable: ${match[1]}. Expected operator.`, this.lineNumber);
                 const varNameRaw = match[1];
                 this._validateVariableExists(varNameRaw); // Check if declared/env
                 tokens.push({ type: 'var', value: varNameRaw.toUpperCase() }); // Store uppercase variable token
                 expectedType = 'operator';
             } else if (match[2]) { // Number
                 if (expectedType !== 'value') throw new ParseError(`Unexpected number: ${match[2]}. Expected operator.`, this.lineNumber);
                 const num = parseInt(match[2], 10);
                 if (isNaN(num)) { // Should not happen with regex, but check
                      throw new ParseError(`Invalid number format in expression: ${match[2]}`, this.lineNumber);
                 }
                 tokens.push({ type: 'num', value: num });
                 expectedType = 'operator';
             } else if (match[3]) { // Operator
                 if (expectedType !== 'operator') throw new ParseError(`Unexpected operator: ${match[3]}. Expected value.`, this.lineNumber);
                 tokens.push({ type: 'op', value: match[3] });
                 expectedType = 'value';
             }
             lastIndex = regex.lastIndex;
         }

         // Check for trailing invalid characters
         if (lastIndex < exprString.length && exprString.substring(lastIndex).trim() !== '') {
              throw new ParseError(`Invalid trailing characters in expression: "${exprString.substring(lastIndex).trim()}"`, this.lineNumber);
         }
         if (tokens.length === 0) throw new ParseError(`Empty expression.`, this.lineNumber);
         // Check if expression ends correctly (must end with a value)
         if (expectedType === 'value') throw new ParseError(`Expression cannot end with an operator.`, this.lineNumber);

         return tokens; // Return token list for runtime evaluation
     }
}