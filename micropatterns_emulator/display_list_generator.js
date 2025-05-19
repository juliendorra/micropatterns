// micropatterns_emulator/display_list_generator.js

/**
 * @typedef {Object} DisplayListItem
 * @property {string} type - The command type (e.g., 'FILL_RECT', 'DRAW_ASSET').
 * @property {Object} logicalParams - Parameters for the command (e.g., { x, y, width, height }).
 * @property {DOMMatrix} transformMatrix - Snapshot of the transformation matrix.
 * @property {number} scaleFactor - Snapshot of the scale factor.
 * @property {string} color - Snapshot of the color.
 * @property {Object | null} fillAsset - Snapshot of the fill asset definition (or null for solid).
 * @property {boolean} isOpaque - Hint for occlusion culling (true if item likely covers its bounds opaquely).
 * @property {number} sourceLine - The original script line number for debugging.
 */

export class DisplayListGenerator {
    constructor(assetsDefinition, environmentDefinition) {
        this.assets = assetsDefinition; // e.g., { "PATTERN_NAME": { width, height, data } }
        this.environment = environmentDefinition; // e.g., { WIDTH: 200, HEIGHT: 200, ... }
        this.displayList = [];
        this._variables = {};
        this._state = {
            color: 'black',
            fillAsset: null,
            scale: 1.0,
            matrix: new DOMMatrix(),
            // inverseMatrix is not strictly needed by generator, but good to keep in sync if state is copied
        };
        this.errors = [];
        this._nextDisplayListItemId = 0; // For unique IDs if needed for debugging
    }

    _initializeStateAndVariables(userVariables) {
        this.displayList = [];
        this.errors = [];
        this._nextDisplayListItemId = 0;

        this._state = {
            color: 'black',
            fillAsset: null,
            scale: 1.0,
            matrix: new DOMMatrix(),
        };

        this._variables = {};
        // Load environment variables (uppercase, no $)
        for (const key in this.environment) {
            this._variables[key.toUpperCase()] = this.environment[key];
        }
        // Load initial user variables (e.g., VAR $x -> $X: 0)
        // These should come from the parser's analysis or a pre-pass.
        // For now, assume userVariables is a map like {'$X': 0, '$Y': 10}
        if (userVariables) {
            for (const key in userVariables) {
                // Ensure key is in $VARNAME format for consistency with _resolveValue
                const varKey = key.startsWith('$') ? key.toUpperCase() : `$${key.toUpperCase()}`;
                this._variables[varKey] = userVariables[key];
            }
        }
    }

    _resolveValue(valueSource, lineNumber) {
        // Handle undefined or null values explicitly
        if (valueSource === undefined || valueSource === null) {
            this.errors.push(`Generator Error (Line ${lineNumber}): Value source is ${valueSource}`);
            return 0; // Fallback for undefined/null
        }
        
        if (typeof valueSource === 'number') {
            return valueSource;
        }
        if (typeof valueSource === 'string') {
            if (valueSource.startsWith('$')) {
                const varNameUpper = valueSource.toUpperCase();
                if (this._variables.hasOwnProperty(varNameUpper)) {
                    return this._variables[varNameUpper];
                } else {
                    // Check environment variables if not in user/script vars
                    // (Parser should ensure $VARNAME format for env vars too)
                    const bareNameUpper = varNameUpper.substring(1);
                    if (this.environment.hasOwnProperty(bareNameUpper)) {
                         return this.environment[bareNameUpper];
                    }
                    this.errors.push(`Generator Error (Line ${lineNumber}): Undefined variable ${valueSource}`);
                    return 0; // Fallback
                }
            }
            // Handle keywords like "SOLID", "BLACK", "WHITE" if they are passed as values
            // For now, assume they are handled by specific command logic.
            // If it's a quoted string from parser, it should have been handled by specific command logic (e.g. FILL NAME="pattern")
        }
        this.errors.push(`Generator Error (Line ${lineNumber}): Cannot resolve value from source: ${JSON.stringify(valueSource)}`);
        return 0; // Fallback
    }

    _evaluateExpression(tokens, lineNumber) {
        if (!tokens || tokens.length === 0) {
            this.errors.push(`Generator Error (Line ${lineNumber}): Empty expression.`);
            return 0;
        }

        const rpn = this._convertToRPN(tokens, lineNumber);
        if (!rpn) return 0;

        const stack = [];
        for (const token of rpn) {
            if (token.type === 'num') {
                stack.push(token.value);
            } else if (token.type === 'var') {
                const value = this._resolveValue(token.value, lineNumber); // token.value is $VARNAME_UPPER
                if (value === undefined) {
                     this.errors.push(`Generator Error (Line ${lineNumber}): Variable ${token.value} undefined in expression.`);
                     return 0; // Error case
                }
                stack.push(value);
            } else if (token.type === 'op') {
                if (stack.length < 2) {
                    this.errors.push(`Generator Error (Line ${lineNumber}): Not enough operands for operator '${token.value}'.`);
                    return 0;
                }
                const b = stack.pop();
                const a = stack.pop();
                switch (token.value) {
                    case '+': stack.push(a + b); break;
                    case '-': stack.push(a - b); break;
                    case '*': stack.push(a * b); break;
                    case '/':
                        if (b === 0) {
                            this.errors.push(`Generator Error (Line ${lineNumber}): Division by zero.`);
                            stack.push(0);
                        } else {
                            stack.push(Math.trunc(a / b));
                        }
                        break;
                    case '%':
                        if (b === 0) {
                            this.errors.push(`Generator Error (Line ${lineNumber}): Modulo by zero.`);
                            stack.push(0);
                        } else {
                            stack.push(a % b);
                        }
                        break;
                    default:
                        this.errors.push(`Generator Error (Line ${lineNumber}): Unknown operator ${token.value}.`);
                        return 0;
                }
            }
        }
        if (stack.length !== 1) {
            this.errors.push(`Generator Error (Line ${lineNumber}): Invalid expression result.`);
            return 0;
        }
        return stack[0];
    }

    _getOperatorPrecedence(op) {
        if (op === '*' || op === '/' || op === '%') return 2;
        if (op === '+' || op === '-') return 1;
        return 0;
    }

    _convertToRPN(tokens, lineNumber) {
        const outputQueue = [];
        const operatorStack = [];
        for (const token of tokens) {
            if (token.type === 'num' || token.type === 'var') {
                outputQueue.push(token);
            } else if (token.type === 'op') {
                while (operatorStack.length > 0 &&
                       this._getOperatorPrecedence(operatorStack[operatorStack.length - 1].value) >= this._getOperatorPrecedence(token.value)) {
                    outputQueue.push(operatorStack.pop());
                }
                operatorStack.push(token);
            } else {
                 this.errors.push(`Generator Error (Line ${lineNumber}): Unknown token type in expression for RPN: ${token.type}`);
                 return null;
            }
        }
        while (operatorStack.length > 0) {
            outputQueue.push(operatorStack.pop());
        }
        return outputQueue;
    }

    _evaluateCondition(condition, lineNumber) {
        if (!condition) {
            this.errors.push(`Generator Error (Line ${lineNumber}): Undefined condition object.`);
            return false;
        }

        let leftVal, rightVal;

        if (condition.type === 'modulo') {
            // For modulo: left is $VARNAME, right can be an expression
            leftVal = this._resolveValue(condition.leftVar, lineNumber); // leftVar is always a single variable string
            if (Array.isArray(condition.right)) {
                rightVal = this._evaluateExpression(condition.right, lineNumber);
            } else {
                rightVal = this._resolveValue(condition.right, lineNumber);
            }
            const literal = condition.literal; // Already a number from parser
            if (literal === 0) {
                this.errors.push(`Generator Error (Line ${lineNumber}): Modulo by zero in condition.`);
                return false;
            }

            let leftOperandSource = condition.leftVar;
            // Fallback check for 'leftvar' (all lowercase) if 'leftVar' (camelCase) is undefined.
            // This handles potential inconsistencies if the parser sometimes uses a different casing.
            if (leftOperandSource === undefined && condition.hasOwnProperty('leftvar')) {
                leftOperandSource = condition.leftvar;
            }

            if (leftOperandSource === undefined) {
                this.errors.push(`Generator Error (Line ${lineNumber}): Left operand variable (e.g., '$INDEX') for modulo condition is missing or undefined in the parsed condition object.`);
                return false; // Cannot evaluate condition if left operand is missing
            }

            const modResult = this._resolveValue(leftOperandSource, lineNumber) % literal;
            switch (condition.operator) {
                case '==': return modResult == rightVal; // Use == for loose comparison as types might differ slightly
                case '!=': return modResult != rightVal;
                case '<': return modResult < rightVal;
                case '>': return modResult > rightVal;
                case '<=': return modResult <= rightVal;
                case '>=': return modResult >= rightVal;
                default:
                    this.errors.push(`Generator Error (Line ${lineNumber}): Unknown operator in modulo condition: ${condition.operator}`);
                    return false;
            }
        } else { // standard
            // For standard conditions, both left and right can be expressions
            if (Array.isArray(condition.left)) {
                leftVal = this._evaluateExpression(condition.left, lineNumber);
            } else {
                leftVal = this._resolveValue(condition.left, lineNumber);
            }

            if (Array.isArray(condition.right)) {
                rightVal = this._evaluateExpression(condition.right, lineNumber);
            } else {
                rightVal = this._resolveValue(condition.right, lineNumber);
            }

            switch (condition.operator) {
                case '==': return leftVal == rightVal;
                case '!=': return leftVal != rightVal;
                case '<': return leftVal < rightVal;
                case '>': return leftVal > rightVal;
                case '<=': return leftVal <= rightVal;
                case '>=': return leftVal >= rightVal;
                default:
                    this.errors.push(`Generator Error (Line ${lineNumber}): Unknown operator in condition: ${condition.operator}`);
                    return false;
            }
        }
    }
    
    _isItemOpaque(itemType, fillAsset) {
        // Heuristic: solid fills are opaque. Patterned fills might be if they don't have '0's
        // or if '0's are also drawn opaquely (which depends on COLOR state for FILL).
        // For simplicity, treat all FILL_RECT/FILL_CIRCLE as opaque for their AABB.
        // DRAW is generally not opaque unless the pattern is a solid block.
        if (itemType === 'FILL_RECT' || itemType === 'FILL_CIRCLE' || itemType === 'FILL_PIXEL') {
            if (!fillAsset) return true; // Solid fill is opaque
            // A pattern fill is opaque if its '0's are drawn as the opposite color (e.g. black for COLOR WHITE)
            // This is complex. A simpler heuristic: if it's a fill command, assume its bounds are opaquely filled.
            return true;
        }
        if (itemType === 'PIXEL') return true; // Single pixel is opaque
        // DRAW_ASSET is opaque only if the asset itself has no '0's (transparent parts)
        // and it's drawn with a solid color. This is harder to determine here.
        // Default to false for DRAW_ASSET unless we have more info.
        return false;
    }


    _processCommands(commands) {
        for (const command of commands) {
            const line = command.line; // Convenience
            switch (command.type) {
                case 'VAR':
                    // Parser stores varName (uppercase, no $) and initialExpression (token list or null)
                    // Variable is added to this._variables by _initializeStateAndVariables or here if initialized
                    if (command.initialExpression) {
                        this._variables[`$${command.varName}`] = this._evaluateExpression(command.initialExpression, line);
                    } else {
                        // If not already set by _initializeStateAndVariables (e.g. from parser's list of all vars)
                        if (!this._variables.hasOwnProperty(`$${command.varName}`)) {
                           this._variables[`$${command.varName}`] = 0;
                        }
                    }
                    break;
                case 'LET':
                    // Parser stores targetVar (uppercase, no $) and expression (token list)
                    this._variables[`$${command.targetVar}`] = this._evaluateExpression(command.expression, line);
                    break;
                case 'COLOR':
                    this._state.color = command.params.NAME.toLowerCase();
                    break;
                case 'FILL':
                    if (command.params.NAME === 'SOLID') {
                        this._state.fillAsset = null;
                    } else {
                        // NAME is uppercase pattern name from parser
                        const asset = this.assets[command.params.NAME];
                        if (asset) {
                            this._state.fillAsset = asset;
                        } else {
                            this.errors.push(`Generator Error (Line ${line}): Pattern "${command.params.NAME}" not defined.`);
                            this._state.fillAsset = null; // Fallback to solid
                        }
                    }
                    break;
                case 'RESET_TRANSFORMS':
                    this._state.scale = 1.0;
                    this._state.matrix = new DOMMatrix();
                    break;
                case 'TRANSLATE':
                    const dx = this._resolveValue(command.params.DX, line);
                    const dy = this._resolveValue(command.params.DY, line);
                    this._state.matrix.translateSelf(dx, dy);
                    break;
                case 'ROTATE':
                    const degrees = this._resolveValue(command.params.DEGREES, line);
                    // Note: DOMMatrix.rotateSelf rotates around the current origin (0,0) of the matrix's coordinate system.
                    // The DSL spec says "sets the *absolute* rotation angle around the current origin".
                    // This implies we might need to reset rotation part of matrix then apply.
                    // For simplicity, and matching typical immediate mode, let's assume cumulative.
                    // If absolute is strictly needed, matrix decomposition/recomposition is required.
                    // The current compiler also does cumulative.
                    this._state.matrix.rotateSelf(degrees);
                    break;
                case 'SCALE':
                    const factor = this._resolveValue(command.params.FACTOR, line);
                    this._state.scale = (factor >= 1) ? factor : 1.0;
                    // Note: DSL says "sets the *absolute* uniform scaling factor".
                    // This._state.scale is applied separately in transformPoint, not directly to matrix here.
                    break;

                case 'PIXEL':
                case 'FILL_PIXEL':
                case 'LINE':
                case 'RECT':
                case 'FILL_RECT':
                case 'CIRCLE':
                case 'FILL_CIRCLE':
                case 'DRAW':
                    const item = {
                        id: this._nextDisplayListItemId++,
                        type: command.type,
                        logicalParams: {},
                        transformMatrix: new DOMMatrix(this._state.matrix),
                        scaleFactor: this._state.scale,
                        color: this._state.color,
                        fillAsset: this._state.fillAsset ? { ...this._state.fillAsset } : null, // Snapshot asset
                        isOpaque: this._isItemOpaque(command.type, this._state.fillAsset),
                        sourceLine: line
                    };
                    
                    // command.params keys are already uppercased by the parser.
                    // No need to check for lowercase 'name'.
                    
                    // Resolve all parameter values
                    for (const key in command.params) {
                        const valueSource = command.params[key];
                        // For DRAW NAME="pattern", NAME is already resolved by parser to uppercase string.
                        // For FILL NAME="pattern", NAME is also resolved.
                        // Other params are values or $vars.
                        if (key === 'NAME' && (command.type === 'DRAW' || command.type === 'FILL')) {
                            if (valueSource === undefined) {
                                this.errors.push(`Generator Error (Line ${line}, Command ${command.type}): Parameter NAME is unexpectedly undefined.`);
                                item.logicalParams[key] = "UNDEFINED_ASSET_NAME"; // Fallback
                            } else {
                                item.logicalParams[key] = valueSource; // Already uppercase string
                            }
                        } else {
                            if (valueSource === undefined) {
                                this.errors.push(`Generator Error (Line ${line}, Command ${command.type}): Parameter ${key} has an undefined value from parser.`);
                                item.logicalParams[key] = 0; // Fallback for undefined numeric/variable param
                            } else {
                                item.logicalParams[key] = this._resolveValue(valueSource, line);
                            }
                        }
                    }
                    this.displayList.push(item);
                    break;

                case 'REPEAT':
                    const count = this._resolveValue(command.count, line);
                    if (typeof count !== 'number' || count < 0) {
                        this.errors.push(`Generator Error (Line ${line}): REPEAT COUNT must be a non-negative number. Got ${count}`);
                        break;
                    }
                    const savedIndex = this._variables['$INDEX'];
                    for (let i = 0; i < count; i++) {
                        this._variables['$INDEX'] = i;
                        this._processCommands(command.commands); // Recurse
                    }
                    this._variables['$INDEX'] = savedIndex; // Restore
                    break;
                case 'IF':
                    const conditionMet = this._evaluateCondition(command.condition, line);
                    if (conditionMet) {
                        this._processCommands(command.thenCommands); // Recurse
                    } else if (command.elseCommands) {
                        this._processCommands(command.elseCommands); // Recurse
                    }
                    break;
                case 'NOOP':
                    break; // Do nothing
                default:
                    this.errors.push(`Generator Error (Line ${line}): Unknown command type "${command.type}"`);
            }
        }
    }

    generate(parsedCommands, initialUserVariables) {
        this._initializeStateAndVariables(initialUserVariables);
        this._processCommands(parsedCommands);
        return {
            displayList: this.displayList,
            errors: this.errors
        };
    }
}