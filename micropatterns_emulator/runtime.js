class MicroPatternsRuntime {
    constructor(ctx, assets, environment, errorCallback) {
        this.ctx = ctx;
        this.assets = assets; // { patterns: {...}, icons: {...} }
        this.environment = environment; // { HOUR, MINUTE, SECOND, COUNTER, WIDTH, HEIGHT }
        this.errorCallback = errorCallback || console.error;
        this.drawing = new MicroPatternsDrawing(ctx); // Drawing primitives instance

        // Precompute sin/cos tables (scaled by 256 for integer math) - Moved to drawing.js
        // this.sinTable = this.drawing.sinTable;
        // this.cosTable = this.drawing.cosTable;

        this.resetState();
    }

    resetState() {
        this.variables = {}; // User-defined variables { name: value }
        this.state = {
            color: 'black', // 'black' or 'white'
            pattern: null, // null (solid) or pattern object from assets
            translateX: 0,
            translateY: 0,
            rotation: 0, // degrees 0-359
            scale: 1, // integer >= 1
        };
         // Clear display with white background at the start of execution
         this.ctx.fillStyle = 'white';
         this.ctx.fillRect(0, 0, this.environment.WIDTH, this.environment.HEIGHT);
    }

    execute(commands) {
        this.resetState();
        try {
            for (const command of commands) {
                this.executeCommand(command);
            }
        } catch (e) {
             // Errors should already be runtime errors with line numbers
             if (e.isRuntimeError) {
                 this.errorCallback(e.message); // Message already includes line number
             } else {
                 // Catch unexpected runtime errors
                 this.errorCallback(`Unexpected Runtime Error: ${e.message}`);
                 console.error("Unexpected Runtime Error Stack:", e.stack);
             }
        }
    }

     // Centralized value resolver
     _resolveValue(valueSource, currentEnv) {
         if (typeof valueSource === 'number') {
             return valueSource; // It's an integer literal
         }
         if (typeof valueSource === 'string') {
             if (valueSource.startsWith('$')) {
                 const varName = valueSource.substring(1);
                 // Check environment variables first (including $INDEX from currentEnv)
                 if (varName === 'HOUR') return currentEnv.HOUR;
                 if (varName === 'MINUTE') return currentEnv.MINUTE;
                 if (varName === 'SECOND') return currentEnv.SECOND;
                 if (varName === 'COUNTER') return currentEnv.COUNTER;
                 if (varName === 'WIDTH') return currentEnv.WIDTH;
                 if (varName === 'HEIGHT') return currentEnv.HEIGHT;
                 if (varName === 'INDEX' && currentEnv.INDEX !== undefined) return currentEnv.INDEX;

                 // Check user variables
                 if (this.variables.hasOwnProperty(varName)) {
                     return this.variables[varName];
                 } else {
                     // This should have been caught by parser, but defensive check
                     throw this.runtimeError(`Undefined variable: ${valueSource}`);
                 }
             } else {
                 // It's likely a string literal (e.g., pattern name, color name)
                 // The parser should have handled validation/normalization
                 return valueSource;
             }
         }
         // Should not happen with valid parsing
         throw this.runtimeError(`Invalid value type encountered: ${typeof valueSource}`);
     }

     executeCommand(command, loopIndex = null) {
         // Make $INDEX available if we are in a loop
         const currentEnv = { ...this.environment };
         if (loopIndex !== null) {
             currentEnv.INDEX = loopIndex;
         }

         // Helper to resolve all parameters in a command using the central resolver
         const resolveParams = (params) => {
             const resolved = {};
             for (const key in params) {
                 // Resolve each parameter value using the current environment context
                 resolved[key] = this._resolveValue(params[key], currentEnv);
             }
             return resolved;
         };

        try {
            // Resolve parameters ONLY if the command has them
            const p = command.params ? resolveParams(command.params) : {};

            switch (command.type) {
                case 'NOOP': // For commands handled at parse time like DEFINE
                    break;

                case 'VAR':
                    // Declaration handled by parser, initialize variable here
                    this.variables[command.varName] = 0;
                    break;

                case 'LET':
                    // Target variable was validated by parser
                    const value = this.evaluateExpression(command.expression, currentEnv);
                    this.variables[command.targetVar] = value;
                    break;

                case 'COLOR':
                    this.state.color = p.NAME.toLowerCase(); // Already validated/normalized by parser
                    break;

                case 'PATTERN':
                    if (p.NAME === 'SOLID') {
                        this.state.pattern = null;
                    } else {
                        const patternName = p.NAME; // Already resolved & unquoted by parser
                        if (!this.assets.patterns[patternName]) {
                            // Should ideally not happen if parser validates against defined assets,
                            // but good runtime check.
                            throw this.runtimeError(`Pattern "${patternName}" not defined.`);
                        }
                        this.state.pattern = this.assets.patterns[patternName];
                    }
                    break;

                case 'RESET_TRANSFORMS':
                    this.state.translateX = 0;
                    this.state.translateY = 0;
                    this.state.rotation = 0;
                    this.state.scale = 1;
                    break;

                case 'TRANSLATE':
                    this.state.translateX += p.DX;
                    this.state.translateY += p.DY;
                    break;

                case 'ROTATE':
                    // Ensure degrees are within 0-359
                    let degrees = p.DEGREES % 360;
                    if (degrees < 0) degrees += 360;
                    this.state.rotation = degrees;
                    break;

                case 'SCALE':
                     // Factor >= 1 validated by parser
                    this.state.scale = p.FACTOR;
                    break;

                // --- Drawing Commands ---
                case 'PIXEL':
                    this.drawing.drawPixel(p.X, p.Y, this.state);
                    break;
                case 'LINE':
                    this.drawing.drawLine(p.X1, p.Y1, p.X2, p.Y2, this.state);
                    break;
                case 'RECT':
                    this.drawing.drawRect(p.X, p.Y, p.WIDTH, p.HEIGHT, this.state);
                    break;
                case 'FILL_RECT':
                    this.drawing.fillRect(p.X, p.Y, p.WIDTH, p.HEIGHT, this.state);
                    break;
                case 'CIRCLE':
                    this.drawing.drawCircle(p.X, p.Y, p.RADIUS, this.state);
                    break;
                case 'FILL_CIRCLE':
                    this.drawing.fillCircle(p.X, p.Y, p.RADIUS, this.state);
                    break;
                case 'ICON':
                    const iconName = p.NAME; // Already resolved & unquoted by parser
                    const iconData = this.assets.icons[iconName];
                    if (!iconData) {
                        throw this.runtimeError(`Icon "${iconName}" not defined.`);
                    }
                    this.drawing.drawIcon(p.X, p.Y, iconData, this.state);
                    break;

                // --- Control Flow ---
                case 'REPEAT':
                    // Resolve count using current environment (in case $INDEX is used inside count)
                    const count = this._resolveValue(command.count, currentEnv);
                    if (!Number.isInteger(count) || count < 0) {
                         throw this.runtimeError(`REPEAT COUNT must resolve to a non-negative integer. Got: ${count}`);
                    }
                    for (let i = 0; i < count; i++) {
                        // Execute nested commands, passing the loop index
                        for (const nestedCmd of command.commands) {
                            // Pass index 'i' to nested command execution
                            this.executeCommand(nestedCmd, i);
                        }
                    }
                    break;

                case 'IF':
                    const conditionMet = this.evaluateCondition(command.condition, currentEnv);
                    const blockToExecute = conditionMet ? command.thenCommands : command.elseCommands;

                    if (blockToExecute) {
                        for (const nestedCmd of blockToExecute) {
                            // Pass loop index if any
                            this.executeCommand(nestedCmd, loopIndex);
                        }
                    }
                    break;

                default:
                    // Should be caught by parser
                    throw this.runtimeError(`Unsupported command type encountered: ${command.type}`);
            }
        } catch (e) {
             // If it's already a runtime error, add line number and rethrow
             // Otherwise, wrap it in a runtime error with the current command's line number
             if (e.isRuntimeError) {
                 throw e; // Already has line number info
             } else {
                  throw this.runtimeError(e.message, command.line);
             }
        }
    }

     // Evaluate parsed expression tokens
     evaluateExpression(tokens, currentEnv) {
         // Resolve values first using the centralized resolver
         const resolvedTokens = tokens.map(token => {
             if (token.type === 'num') return token.value;
             if (token.type === 'var') return this._resolveValue(token.value, currentEnv);
             if (token.type === 'op') return token.value; // Keep operators as is
             throw this.runtimeError(`Invalid token type in expression: ${token.type}`);
         });

         // Separate values and operators
         let values = resolvedTokens.filter(t => typeof t === 'number');
         let ops = resolvedTokens.filter(t => typeof t === 'string'); // Operators

         // Helper for applying an operation
         const applyOp = (val1, op, val2) => {
             switch (op) {
                 case '+': return val1 + val2;
                 case '-': return val1 - val2;
                 case '*': return val1 * val2;
                 case '/':
                     if (val2 === 0) throw this.runtimeError(`Division by zero.`);
                     return Math.trunc(val1 / val2); // Integer division
                 case '%':
                     if (val2 === 0) throw this.runtimeError(`Modulo by zero.`);
                     return val1 % val2;
                 default: throw this.runtimeError(`Unknown operator in expression: ${op}`);
             }
         };

         // Phase 1: Multiplication, Division, Modulo (left-to-right)
         let i = 0;
         while (i < ops.length) {
             if (ops[i] === '*' || ops[i] === '/' || ops[i] === '%') {
                 values[i] = applyOp(values[i], ops[i], values[i + 1]);
                 values.splice(i + 1, 1); // Remove right operand value
                 ops.splice(i, 1); // Remove operator
             } else {
                 i++; // Move to next operator
             }
         }

         // Phase 2: Addition, Subtraction (left-to-right)
         // Now only '+' and '-' remain, process strictly left-to-right
         while (ops.length > 0) {
             values[0] = applyOp(values[0], ops[0], values[1]);
             values.splice(1, 1); // Remove right operand value
             ops.splice(0, 1); // Remove operator
         }

         if (values.length !== 1) {
             // Should not happen if parsing was correct
             throw this.runtimeError(`Expression evaluation failed. Remaining values: ${values.length}`);
         }
         return values[0]; // Final result
     }


     // Evaluate parsed condition object
     evaluateCondition(condition, currentEnv) {
         let leftVal, rightVal;

         // Resolve values using the central resolver
         const resolve = (val) => this._resolveValue(val, currentEnv);

         if (condition.type === 'modulo') {
             const varValue = resolve(condition.leftVar);
             const literal = condition.literal; // Literal is already a number from parser
             const rightRaw = resolve(condition.right);

             if (!Number.isInteger(varValue)) throw this.runtimeError(`Left side of modulo must be an integer. Got: ${varValue}`);
             if (!Number.isInteger(rightRaw)) throw this.runtimeError(`Right side of modulo comparison must be an integer. Got: ${rightRaw}`);
             if (literal === 0) throw this.runtimeError(`Modulo by zero in condition.`);

             leftVal = varValue % literal;
             rightVal = rightRaw;
         } else { // standard condition
             leftVal = resolve(condition.left);
             rightVal = resolve(condition.right);

             // Ensure both sides resolved to numbers for comparison
             if (typeof leftVal !== 'number' || typeof rightVal !== 'number') {
                  throw this.runtimeError(`Cannot compare non-numeric values in condition: ${leftVal} vs ${rightVal}`);
             }
         }

         switch (condition.operator) {
             case '==': return leftVal === rightVal;
             case '!=': return leftVal !== rightVal;
             case '>': return leftVal > rightVal;
             case '<': return leftVal < rightVal;
             case '>=': return leftVal >= rightVal;
             case '<=': return leftVal <= rightVal;
             default: throw this.runtimeError(`Unknown operator in condition: ${condition.operator}`);
         }
     }

     // Helper to create runtime errors with line numbers
     runtimeError(message, lineNumber) {
         // Ensure lineNumber is attached if available
         const lineInfo = lineNumber ? ` (Line ${lineNumber})` : '';
         const error = new Error(`Runtime Error${lineInfo}: ${message}`);
         error.lineNumber = lineNumber;
         error.isRuntimeError = true; // Mark as runtime error
         return error;
     }
}