// micropatterns_emulator/compiler.js

export class MicroPatternsCompiler {
    constructor() {
        this.errors = [];
        this.declaredVariables = new Set(); // Tracks variables declared with VAR ($VARNAME_UPPER)
        this.assetsDefinition = null; // Will be set during compile
        this.environmentDefinition = null; // Will be set during compile
    }

    _resetForCompilation(assetsDef, envDef) {
        this.errors = [];
        this.declaredVariables = new Set();
        this.assetsDefinition = assetsDef;
        this.environmentDefinition = envDef; // Store for context if needed by helpers
    }

    _isEnvVar(varNameUpper) { // varNameUpper is without $
        const envVars = ['HOUR', 'MINUTE', 'SECOND', 'COUNTER', 'WIDTH', 'HEIGHT', 'INDEX'];
        return envVars.includes(varNameUpper);
    }

    _generateValueCode(valueSource, lineNumber) {
        // Handle undefined values explicitly with more context
        if (valueSource === undefined) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Cannot generate code for undefined parameter value`);
            console.warn(`Compiler warning: undefined value at line ${lineNumber}, using fallback value 0`);
            // Instead of just returning '0', generate a runtime check that provides better error context
            return `(function() {
                if (typeof _errorReportedForLine${lineNumber} === 'undefined') {
                    console.error("Runtime encountered undefined value at line ${lineNumber}");
                    _errorReportedForLine${lineNumber} = true;
                }
                return 0;
            })()`;
        }
        
        if (valueSource === null) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Cannot generate code for null parameter value`);
            return '0'; // Fallback for null
        }
        
        if (typeof valueSource === 'number') {
            // Ensure it's a valid number
            if (isNaN(valueSource)) {
                this.errors.push(`Compiler Error (Line ${lineNumber}): NaN value detected`);
                return '0';
            }
            return String(valueSource);
        }
        
        if (typeof valueSource === 'string') {
            if (valueSource.startsWith('$')) {
                // Handle variable references with additional runtime safety
                const varKey = valueSource.toUpperCase();
                // Generate code that checks for undefined variables at runtime
                return `(function() {
                    const value = _variables['${varKey}'];
                    if (value === undefined) {
                        _runtimeError(\`Variable ${varKey} is undefined at line ${lineNumber}\`, ${lineNumber});
                        return 0;
                    }
                    return value;
                })()`;
            } else {
                // Handle other string values - could be a keyword or literal
                // For now, just return as string literal
                return `"${valueSource}"`;
            }
        }
        
        // Handle object values (like expression tokens) or other complex types
        if (typeof valueSource === 'object') {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Cannot directly generate code for object value. Expected primitive or variable reference.`);
            console.warn(`Complex value at line ${lineNumber}:`, valueSource);
            return '0';
        }
        
        // Fallback for any other type
        this.errors.push(`Compiler Error (Line ${lineNumber}): Cannot generate code for value of type ${typeof valueSource}: ${JSON.stringify(valueSource)}`);
        return '0';
    }

    _generateExpressionCode(tokens, lineNumber) {
        if (!tokens || tokens.length === 0) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Empty expression tokens.`);
            return '0';
        }

        // Convert to RPN for proper operator precedence handling
        const rpn = this._convertToRPN(tokens, lineNumber);
        if (!rpn) {
            // Error already logged in _convertToRPN
            return '0';
        }

        try {
            const stack = [];
            for (const token of rpn) {
                if (token.type === 'num') {
                    stack.push(String(token.value));
                } else if (token.type === 'var') {
                    // Add a runtime check for variable existence
                    const varKey = token.value.toUpperCase();
                    stack.push(`((_variables['${varKey}'] !== undefined) ? _variables['${varKey}'] : (function() { _runtimeError(\`Variable ${varKey} is undefined in expression at line ${lineNumber}\`, ${lineNumber}); return 0; })())`);
                } else if (token.type === 'op') {
                    if (stack.length < 2) {
                        this.errors.push(`Compiler Error (Line ${lineNumber}): Not enough operands for operator '${token.value}' in expression.`);
                        return '0';
                    }
                    
                    const b = stack.pop();
                    const a = stack.pop();
                    
                    if (token.value === '/') {
                        stack.push(`_intDiv(${a}, ${b})`);
                    } else if (token.value === '%') {
                        stack.push(`_intMod(${a}, ${b})`);
                    } else {
                        // For other operations, add runtime safety checks
                        if (token.value === '+' || token.value === '-' || token.value === '*') {
                            // Wrap expressions in safety check functions
                            stack.push(`(function() {
                                const aVal = ${a};
                                const bVal = ${b};
                                if (aVal === undefined) {
                                    _runtimeError(\`Left operand for '${token.value}' is undefined at line ${lineNumber}\`, ${lineNumber});
                                    return 0;
                                }
                                if (bVal === undefined) {
                                    _runtimeError(\`Right operand for '${token.value}' is undefined at line ${lineNumber}\`, ${lineNumber});
                                    return 0;
                                }
                                return aVal ${token.value} bVal;
                            })()`);
                        } else {
                            stack.push(`(${a} ${token.value} ${b})`);
                        }
                    }
                }
            }
            
            if (stack.length !== 1) {
                this.errors.push(`Compiler Error (Line ${lineNumber}): Expression did not resolve to a single value. Stack: ${stack.join(', ')}`);
                return '0';
            }
            
            return stack[0];
        } catch (e) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Exception during expression code generation: ${e.message}`);
            return '0';
        }
    }
    
    _getOperatorPrecedence(op) {
        if (op === '*' || op === '/' || op === '%') return 2;
        if (op === '+' || op === '-') return 1;
        return 0;
    }

    _convertToRPN(tokens, lineNumber) {
        if (!tokens || tokens.length === 0) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Empty token list for RPN conversion`);
            return null;
        }
        
        const outputQueue = [];
        const operatorStack = [];
        
        try {
            for (const token of tokens) {
                if (!token || !token.type) {
                    this.errors.push(`Compiler Error (Line ${lineNumber}): Invalid token in expression: ${JSON.stringify(token)}`);
                    return null;
                }
                
                if (token.type === 'num' || token.type === 'var') {
                    outputQueue.push(token);
                } else if (token.type === 'op') {
                    while (operatorStack.length > 0 &&
                           this._getOperatorPrecedence(operatorStack[operatorStack.length - 1].value) >= this._getOperatorPrecedence(token.value)) {
                        outputQueue.push(operatorStack.pop());
                    }
                    operatorStack.push(token);
                } else {
                     this.errors.push(`Compiler Error (Line ${lineNumber}): Unknown token type in expression for RPN: ${token.type}`);
                     return null;
                }
            }
            
            while (operatorStack.length > 0) {
                outputQueue.push(operatorStack.pop());
            }
            
            // Validate the RPN output - should have exactly one more operand than operators
            let operandCount = 0;
            let operatorCount = 0;
            
            for (const token of outputQueue) {
                if (token.type === 'num' || token.type === 'var') operandCount++;
                else if (token.type === 'op') operatorCount++;
            }
            
            if (operandCount !== operatorCount + 1) {
                this.errors.push(`Compiler Error (Line ${lineNumber}): Invalid expression structure. Check for missing operands or operators.`);
                return null;
            }
            
            return outputQueue;
        } catch (e) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Exception during RPN conversion: ${e.message}`);
            return null;
        }
    }


    _generateConditionCode(condition, lineNumber) {
        if (!condition) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Invalid or undefined condition object`);
            return 'false';
        }

        try {
            if (condition.type === 'modulo') {
                // Handle special modulo condition format: $var % literal op value
                if (!condition.leftVar) {
                    this.errors.push(`Compiler Error (Line ${lineNumber}): Missing leftVar in modulo condition`);
                    return 'false';
                }
                
                // Wrap in safety function to check for undefined values
                const leftVarCode = this._generateValueCode(condition.leftVar, lineNumber);
                const literalValue = condition.literal || 1; // Default to 1 if missing to avoid errors
                const rightCode = this._generateValueCode(condition.right, lineNumber);
                
                return `(function() {
                    const leftVal = ${leftVarCode};
                    if (leftVal === undefined) {
                        _runtimeError(\`Left variable in modulo condition is undefined at line ${lineNumber}\`, ${lineNumber});
                        return false;
                    }
                    
                    const rightVal = ${rightCode};
                    if (rightVal === undefined) {
                        _runtimeError(\`Right value in modulo condition is undefined at line ${lineNumber}\`, ${lineNumber});
                        return false;
                    }
                    
                    return _intMod(leftVal, ${literalValue}) ${condition.operator} rightVal;
                })()`;
            } else { // standard condition: left op right
                if (!condition.left || !condition.right) {
                    this.errors.push(`Compiler Error (Line ${lineNumber}): Missing operands in standard condition`);
                    return 'false';
                }
                
                const leftCode = this._generateValueCode(condition.left, lineNumber);
                const rightCode = this._generateValueCode(condition.right, lineNumber);
                
                // Wrap in a safety function to check for undefined
                return `(function() {
                    const leftVal = ${leftCode};
                    if (leftVal === undefined) {
                        _runtimeError(\`Left operand in condition is undefined at line ${lineNumber}\`, ${lineNumber});
                        return false;
                    }
                    
                    const rightVal = ${rightCode};
                    if (rightVal === undefined) {
                        _runtimeError(\`Right operand in condition is undefined at line ${lineNumber}\`, ${lineNumber});
                        return false;
                    }
                    
                    return leftVal ${condition.operator} rightVal;
                })()`;
            }
        } catch (e) {
            this.errors.push(`Compiler Error (Line ${lineNumber}): Error generating condition code: ${e.message}`);
            return 'false'; // Default to false on error
        }
    }

    _compileCommandToJs(command) {
        let js = `// Line ${command.line}: ${command.type}`;
        switch (command.type) {
            case 'VAR':
                // VARs without initializers are handled by populating initialUserVariables.
                // VARs with initializers are set directly in the compiled function's _variables.
                // Parser adds varName (uppercase, no $) to command.
                // Parser adds initialExpression (token list or null) to command.
                this.declaredVariables.add(command.varName); // Track declaration
                if (command.initialExpression) {
                    js += `\n_variables['$${command.varName.toUpperCase()}'] = ${this._generateExpressionCode(command.initialExpression, command.line)};`;
                } else {
                    // This variable will be initialized from _initialUserVariables map
                    // which is pre-filled by the loop before generating the function body.
                    js += `\n/* VAR $${command.varName} initialized from _initialUserVariables */`;
                }
                break;
            case 'LET':
                js += `\n_variables['$${command.targetVar.toUpperCase()}'] = ${this._generateExpressionCode(command.expression, command.line)};`;
                break;
            case 'COLOR':
                js += `\n_state.color = '${command.params.NAME.toLowerCase()}';`;
                break;
            case 'FILL':
                if (command.params.NAME === 'SOLID') {
                    js += `\n_state.fillAsset = null;`;
                } else {
                    const patternNameUpper = command.params.NAME.toUpperCase();
                    js += `\n_state.fillAsset = _assets['${patternNameUpper}'];`;
                    js += `\nif (!_state.fillAsset) { _runtimeError("Pattern '${patternNameUpper}' not defined.", ${command.line}); }`;
                }
                break;
            case 'RESET_TRANSFORMS':
                js += `\n_state.scale = 1.0; _state.matrix = new DOMMatrix(); _state.inverseMatrix = new DOMMatrix();`;
                break;
            case 'TRANSLATE':
                const dx = this._generateValueCode(command.params.DX, command.line);
                const dy = this._generateValueCode(command.params.DY, command.line);
                js += `\n_state.matrix.translateSelf(${dx}, ${dy}); _state.inverseMatrix = _state.matrix.inverse();`;
                break;
            case 'ROTATE':
                const degrees = this._generateValueCode(command.params.DEGREES, command.line);
                js += `\n_state.matrix.rotateSelf(${degrees}); _state.inverseMatrix = _state.matrix.inverse();`;
                break;
            case 'SCALE':
                const factor = this._generateValueCode(command.params.FACTOR, command.line);
                js += `\n_state.scale = (${factor} >= 1) ? ${factor} : 1.0;`;
                break;
            case 'PIXEL':
                js += `\n_drawing.drawPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                break;
            case 'LINE':
                js += `\n_drawing.drawLine(${this._generateValueCode(command.params.X1, command.line)}, ${this._generateValueCode(command.params.Y1, command.line)}, ${this._generateValueCode(command.params.X2, command.line)}, ${this._generateValueCode(command.params.Y2, command.line)}, _state);`;
                break;
            case 'RECT':
                js += `\n_drawing.drawRect(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, ${this._generateValueCode(command.params.WIDTH, command.line)}, ${this._generateValueCode(command.params.HEIGHT, command.line)}, _state);`;
                break;
            case 'FILL_RECT':
                js += `\n_drawing.fillRect(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, ${this._generateValueCode(command.params.WIDTH, command.line)}, ${this._generateValueCode(command.params.HEIGHT, command.line)}, _state);`;
                break;
            case 'CIRCLE':
                js += `\n_drawing.drawCircle(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, ${this._generateValueCode(command.params.RADIUS, command.line)}, _state);`;
                break;
            case 'FILL_CIRCLE':
                js += `\n_drawing.fillCircle(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, ${this._generateValueCode(command.params.RADIUS, command.line)}, _state);`;
                break;
            case 'DRAW':
                const drawPatternName = command.params.NAME.toUpperCase();
                js += `\nconst _drawAsset = _assets['${drawPatternName}'];`;
                js += `\nif (!_drawAsset) { _runtimeError("Pattern '${drawPatternName}' not defined for DRAW.", ${command.line}); }`;
                js += `\nelse { _drawing.drawAsset(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _drawAsset, _state); }`;
                break;
            case 'FILL_PIXEL':
                js += `\n_drawing.drawFilledPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                break;
            case 'REPEAT':
                const count = this._generateValueCode(command.count, command.line);
                js += `\nconst _repeatCount_${command.line} = ${count};`;
                js += `\nif (!Number.isInteger(_repeatCount_${command.line}) || _repeatCount_${command.line} < 0) { _runtimeError("REPEAT COUNT must resolve to a non-negative integer. Got: " + _repeatCount_${command.line}, ${command.line}); }`;
                
                // Save the current $INDEX value to restore after the loop
                js += `\nconst _savedIndex_${command.line} = _variables['$INDEX'];`;
                
                js += `\nfor (let _i_${command.line} = 0; _i_${command.line} < _repeatCount_${command.line}; _i_${command.line}++) {`;
                js += `\n  // Set INDEX for this iteration`;
                js += `\n  _variables['$INDEX'] = _i_${command.line};`;
                
                // Add runtime validation that $INDEX is properly set for each iteration
                js += `\n  if (_variables['$INDEX'] === undefined) {`;
                js += `\n    console.warn("INDEX variable undefined in REPEAT at line ${command.line}, iteration " + _i_${command.line});`;
                js += `\n    _variables['$INDEX'] = _i_${command.line}; // Force correction`;
                js += `\n  }`;
                
                js += this._compileCommandsToString(command.commands);
                js += `\n}`;
                
                // Restore the previous $INDEX value after the loop
                js += `\n_variables['$INDEX'] = _savedIndex_${command.line};`;
                break;
            case 'IF':
                // Add validation for the condition object
                if (!command.condition) {
                    this.errors.push(`Compiler Error (Line ${command.line}): Missing condition in IF statement`);
                    js += `\n_runtimeError("Missing condition in IF statement", ${command.line});`;
                    break;
                }
                
                // Log the condition object for debugging
                console.log(`Compiling IF condition at line ${command.line}:`, command.condition);
                
                try {
                    const conditionCode = this._generateConditionCode(command.condition, command.line);
                    js += `\n// IF condition from line ${command.line}`;
                    js += `\nif (${conditionCode}) {`;
                    js += this._compileCommandsToString(command.thenCommands);
                    js += `\n}`;
                    if (command.elseCommands && command.elseCommands.length > 0) {
                        js += ` else {`;
                        js += this._compileCommandsToString(command.elseCommands);
                        js += `\n}`;
                    }
                } catch (e) {
                    this.errors.push(`Compiler Error (Line ${command.line}): Failed to compile IF condition: ${e.message}`);
                    js += `\n_runtimeError("Failed to compile IF condition: ${e.message}", ${command.line});`;
                }
                break;
            case 'NOOP': // e.g. DEFINE PATTERN
                js += `\n/* NOOP: ${command.type} */`;
                break;
            default:
                this.errors.push(`Compiler Error (Line ${command.line}): Unsupported command type '${command.type}'`);
                js += `\n_runtimeError("Unsupported command: ${command.type}", ${command.line});`;
        }
        return js;
    }

    _compileCommandsToString(commands) {
        if (!commands || commands.length === 0) return "";
        let blockJs = "";
        for (const cmd of commands) {
            blockJs += `\n${this._compileCommandToJs(cmd)}`;
        }
        return blockJs;
    }

    compile(parsedCommands, assetsDefinition, environmentDefinition) {
        console.log("Starting compilation with", parsedCommands.length, "commands");
        this._resetForCompilation(assetsDefinition, environmentDefinition);
        const jsCodeLines = [];
        const initialUserVariables = {}; // For VAR declarations without initializers ($VAR_UPPER: 0)

        // First pass for VAR initializations to populate initialUserVariables
        for (const command of parsedCommands) {
            if (command.type === 'VAR' && !command.initialExpression) {
                initialUserVariables[`$${command.varName.toUpperCase()}`] = 0;
            }
            // Also collect all declared variables for scope checking if needed later
            if (command.type === 'VAR') {
                this.declaredVariables.add(command.varName.toUpperCase());
            }
        }
        
        console.log("Declared variables:", Array.from(this.declaredVariables));
        console.log("Initial user variables:", initialUserVariables);
        
        // Second pass to generate code for all commands
        for (const command of parsedCommands) {
            try {
                // Log IF commands for debugging
                if (command.type === 'IF') {
                    console.log(`Processing IF command at line ${command.line}:`, command);
                }
                
                const commandJs = this._compileCommandToJs(command);
                jsCodeLines.push(commandJs);
            } catch (e) {
                console.error(`Error compiling command at line ${command.line}:`, command, e);
                this.errors.push(`Compiler Error (Line ${command.line}): ${e.message}`);
                jsCodeLines.push(`\n_runtimeError("Compiler error: ${e.message}", ${command.line});`);
            }
        }

        const functionBody = `
            // --- Compiled MicroPatterns Script ---
            // Arguments: _environment, _drawing, _assets, _initialUserVariables
            
            // Initialize _variables as an empty object
            let _variables = {};
            
            // Iterate through _environment keys and add them with $ prefix and uppercase
            for (const key in _environment) {
                _variables['$' + key.toUpperCase()] = _environment[key];
            }
            
            // Merge _initialUserVariables (which already have the $VAR_UPPER format)
            for (const key in _initialUserVariables) {
                _variables[key] = _initialUserVariables[key];
            }

            let _state = {
                color: 'black',
                fillAsset: null,
                scale: 1.0,
                matrix: new DOMMatrix(),
                inverseMatrix: new DOMMatrix()
            };

            const _runtimeError = (message, lineNumber) => {
                const lineInfo = (typeof lineNumber === 'number' && lineNumber > 0) ? \` (Line \${lineNumber})\` : '';
                
                // Add variable state inspection to error message for better debugging
                let variableState = '';
                try {
                    variableState = '\\nVariable state: ' +
                        Object.entries(_variables)
                            .filter(([key]) => key.startsWith('$'))
                            .map(([key, val]) => \`\${key}: \${val === undefined ? 'undefined' : val}\`)
                            .join(', ');
                } catch (e) {
                    variableState = '\\nCould not inspect variables: ' + e.message;
                }
                
                const err = new Error(\`Compiled Script Error\${lineInfo}: \${message}\${variableState}\`);
                err.isCompiledRuntimeError = true;
                err.lineNumber = lineNumber;
                err.variables = {..._variables}; // Attach variable state to error object
                throw err;
            };

            const _intDiv = (a, b) => {
                // First check for undefined or null operands explicitly
                if (a === undefined) {
                    _runtimeError("First operand for division is undefined", "expression");
                    return 0;
                }
                if (b === undefined) {
                    _runtimeError("Second operand for division is undefined", "expression");
                    return 0;
                }
                
                // Convert inputs to numbers if possible
                const numA = Number(a);
                const numB = Number(b);
                
                // Check for division by zero
                if (numB === 0) {
                    _runtimeError("Division by zero.", "expression");
                    return 0;
                }
                
                // Check for invalid operands
                if (isNaN(numA) || isNaN(numB)) {
                    _runtimeError('Operands for division must be numbers (and not NaN). Got: ' + a + ' and ' + b, "expression");
                    return 0;
                }
                
                return Math.trunc(numA / numB);
            };
            
            const _intMod = (a, b) => {
                // First check for undefined or null operands explicitly
                if (a === undefined) {
                    _runtimeError("First operand for modulo is undefined", "expression");
                    return 0;
                }
                if (b === undefined) {
                    _runtimeError("Second operand for modulo is undefined", "expression");
                    return 0;
                }
                
                // Convert inputs to numbers if possible
                const numA = Number(a);
                const numB = Number(b);
                
                // Check for modulo by zero
                if (numB === 0) {
                    _runtimeError("Modulo by zero.", "expression");
                    return 0;
                }
                
                // Check for invalid operands
                if (isNaN(numA) || isNaN(numB)) {
                    _runtimeError('Operands for modulo must be numbers (and not NaN). Got: ' + a + ' and ' + b, "expression");
                    return 0;
                }
                
                return numA % numB;
            };

            // --- Start of user script logic ---
            try {
                ${jsCodeLines.join('\n')}
            } catch (e) {
                if (e.isCompiledRuntimeError) throw e; // Re-throw our specific errors
                // Catch unexpected JS errors from generated code
                console.error("Unexpected JS error in compiled script:", e);
                _runtimeError("Unexpected internal error: " + e.message, e.lineNumber || "unknown");
            }
            // --- End of user script logic ---
        `;

        if (this.errors.length > 0) {
            console.warn("Compilation finished with errors:", this.errors);
            // Optionally, don't attempt to create function if critical errors exist
        }

        try {
            const compiledFunction = new Function('_environment', '_drawing', '_assets', '_initialUserVariables', functionBody);
            return {
                execute: compiledFunction,
                initialVariables: initialUserVariables, // Pass this to runner
                errors: this.errors
            };
        } catch (e) {
            this.errors.push(`Compiler Error: Failed to create function from generated code - ${e.message}. Code:\n${functionBody}`);
            console.error("Error creating compiled function:", e, functionBody);
            return {
                execute: () => {
                    const errorMsg = "Compilation failed, cannot execute. Check console for details.";
                    console.error(errorMsg);
                    if (typeof _runtimeError === 'function') _runtimeError(errorMsg, 0); // If _runtimeError is available in scope
                    else throw new Error(errorMsg);
                },
                initialVariables: {},
                errors: this.errors
            };
        }
    }
}