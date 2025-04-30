class MicroPatternsRuntime {
    constructor(ctx, assets, environment, errorCallback) {
        this.ctx = ctx;
        this.assets = assets; // { assets: {UPPER_NAME: data} } - Unified storage
        this.environment = environment; // { HOUR, MINUTE, SECOND, COUNTER, WIDTH, HEIGHT }
        this.errorCallback = errorCallback || console.error;
        this.drawing = new MicroPatternsDrawing(ctx); // Drawing primitives instance

        this.resetState();
    }

    resetState() {
        // User variables stored with UPPERCASE names as keys
        this.variables = {}; // { $UPPER_VAR_NAME: value }
        this.state = {
            color: 'black', // 'black' or 'white'
            fillAsset: null, // null (solid) or asset object from assets.assets
            transformations: [], // Stores sequence of {type: 'translate'/'rotate', ...}
            scale: 1, // Stores the *last* set absolute scale factor
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
     // Expects valueSource to be number or variable reference string (e.g., "$MYVAR" - already uppercase from parser)
     // lineNumber is crucial for accurate error reporting.
     _resolveValue(valueSource, currentEnv, lineNumber) {
         if (typeof valueSource === 'number') {
             return valueSource; // It's an integer literal
         }
          if (typeof valueSource === 'string' && valueSource.startsWith('$')) {
              // Variable name expected to be uppercase from parser ($VARNAME)
              const varNameUpper = valueSource; // Already includes '$'

              // Check environment variables first (using uppercase names)
              if (varNameUpper === '$HOUR') return currentEnv.HOUR;
              if (varNameUpper === '$MINUTE') return currentEnv.MINUTE;
              if (varNameUpper === '$SECOND') return currentEnv.SECOND;
              if (varNameUpper === '$COUNTER') return currentEnv.COUNTER;
              if (varNameUpper === '$WIDTH') return currentEnv.WIDTH;
              if (varNameUpper === '$HEIGHT') return currentEnv.HEIGHT;
              if (varNameUpper === '$INDEX' && currentEnv.INDEX !== undefined) return currentEnv.INDEX;

              // Check user variables (using uppercase names)
              if (this.variables.hasOwnProperty(varNameUpper)) {
                  return this.variables[varNameUpper];
              } else {
                  // This should have been caught by parser, but defensive check
                  throw this.runtimeError(`Undefined variable: ${valueSource}`, lineNumber);
              }
          } else {
              // Handle cases where valueSource is not number or variable string
              // (e.g. keywords like SOLID passed incorrectly, or just invalid type)
              throw this.runtimeError(`Cannot resolve value: Expected number or variable ($VAR), got ${valueSource} (type: ${typeof valueSource})`, lineNumber);
          }
     }

     executeCommand(command, loopIndex = null) {
         // Make $INDEX available if we are in a loop
         const currentEnv = { ...this.environment };
         if (loopIndex !== null) {
             // Store $INDEX as uppercase key consistent with other env vars
             currentEnv.INDEX = loopIndex;
         }

         // Helper to resolve all parameters in a command using the central resolver
         // Parameter values can be numbers or variable references ($VARNAME - uppercase)
         // Skips resolving specific keys like NAME for COLOR/FILL/DRAW.
         const resolveParams = (params, commandType, lineNumber) => {
             const resolved = {};
             // Skip resolving NAME for commands that use it as an identifier/keyword
             const skipResolveKeys = (commandType === 'COLOR' || commandType === 'FILL' || commandType === 'DRAW') ? ['NAME'] : [];

             for (const key in params) {
                 if (skipResolveKeys.includes(key)) {
                     // Pass the original parsed value (keyword or identifier string) directly
                     resolved[key] = params[key];
                 } else {
                     // Resolve other parameter values using the current environment context
                     // Pass the uppercase variable reference (e.g., $MYVAR) or number directly
                     resolved[key] = this._resolveValue(params[key], currentEnv, lineNumber);
                 }
             }
             return resolved;
         };

        try {
            // Resolve parameters ONLY if the command has them and is not VAR/LET/IF/REPEAT
            // These special commands handle their own value resolution/parsing logic
            // COLOR, FILL, DRAW are now handled by resolveParams skipping NAME
            let p = {};
            // Added FILL_PIXEL to standard commands
            const standardParamCommands = ['PIXEL', 'LINE', 'RECT', 'FILL_RECT', 'CIRCLE', 'FILL_CIRCLE', 'DRAW', 'TRANSLATE', 'ROTATE', 'SCALE', 'COLOR', 'FILL', 'RESET_TRANSFORMS', 'FILL_PIXEL'];
            if (command.params && standardParamCommands.includes(command.type)) {
                 // Pass command type and line number for context and error reporting
                 p = resolveParams(command.params, command.type, command.line);
            }


            switch (command.type) {
                case 'NOOP': // For commands handled at parse time like DEFINE PATTERN
                    break;
                 case 'VAR':
                     // Declaration handled by parser. Initialize variable here.
                     // command.varName is already uppercase from parser (e.g., "MYVAR")
                     const varRef = '$' + command.varName; // e.g., $MYVAR

                     if (command.initialExpression) {
                         // Evaluate the initial expression using current environment
                         // Pass command line number for evaluation errors
                         const initialValue = this.evaluateExpression(command.initialExpression, currentEnv, command.line);
                         this.variables[varRef] = initialValue;
                     } else {
                         // No initial expression provided, default to 0
                         this.variables[varRef] = 0;
                     }
                     break;

                 case 'LET':
                     // Target variable was validated by parser
                     // command.targetVar is already uppercase from parser (e.g., "MYVAR")
                     // command.expression contains tokens with uppercase variable names (e.g., {type:'var', value:'$OTHERVAR'})
                     const value = this.evaluateExpression(command.expression, currentEnv, command.line);
                     // Assign to uppercase variable reference ('$' prefix)
                     this.variables['$' + command.targetVar] = value;
                     break;

                 case 'COLOR':
                     // p.NAME was passed directly by resolveParams, it's 'BLACK' or 'WHITE' (uppercase)
                     this.state.color = p.NAME.toLowerCase(); // Store as lowercase 'black'/'white' for canvas
                     break;

                 case 'FILL': // Replaces old PATTERN state command
                     // p.NAME was passed directly by resolveParams, it's 'SOLID' or an uppercase pattern name
                     if (p.NAME === 'SOLID') {
                         this.state.fillAsset = null;
                     } else {
                         const patternNameUpper = p.NAME; // Already uppercase from parser
                         // Look up in the unified assets.assets store
                         const assetData = this.assets.assets[patternNameUpper];
                         if (!assetData) {
                             // Use command.line for error reporting
                             throw this.runtimeError(`Pattern "${patternNameUpper}" not defined (check DEFINE PATTERN).`, command.line);
                         }
                         this.state.fillAsset = assetData;
                     }
                     break;

                case 'RESET_TRANSFORMS':
                    this.state.transformations = []; // Clear sequence
                    this.state.scale = 1; // Reset scale factor
                    break;

                case 'TRANSLATE':
                    // p.DX, p.DY are resolved numbers
                    // Add to the transformation sequence
                    this.state.transformations.push({ type: 'translate', dx: p.DX, dy: p.DY });
                    break;

                case 'ROTATE':
                    // p.DEGREES is resolved number
                    let degrees = p.DEGREES % 360;
                    if (degrees < 0) degrees += 360;
                    // Add to the transformation sequence
                    this.state.transformations.push({ type: 'rotate', degrees: degrees });
                    break;

                case 'SCALE':
                    // p.FACTOR is resolved number >= 1
                    // Set the absolute scale factor, replacing the previous one
                    this.state.scale = p.FACTOR;
                    // Note: SCALE does not add to the transformations array itself,
                    // its effect is applied first in transformPoint based on the final state.scale.
                    break;

                // --- Drawing Commands (use resolved parameters p) ---
                case 'PIXEL':
                    this.drawing.drawPixel(p.X, p.Y, this.state);
                    break;
                case 'FILL_PIXEL': // New command
                    // Uses state.fillAsset internally via _getFillAssetPixelColor
                    this.drawing.drawFilledPixel(p.X, p.Y, this.state);
                    break;
                case 'LINE':
                    this.drawing.drawLine(p.X1, p.Y1, p.X2, p.Y2, this.state);
                    break;
                case 'RECT':
                    this.drawing.drawRect(p.X, p.Y, p.WIDTH, p.HEIGHT, this.state);
                    break;
                case 'FILL_RECT':
                    // Uses state.fillAsset internally via getFillAssetPixel
                    this.drawing.fillRect(p.X, p.Y, p.WIDTH, p.HEIGHT, this.state);
                    break;
                case 'CIRCLE':
                    this.drawing.drawCircle(p.X, p.Y, p.RADIUS, this.state);
                    break;
                case 'FILL_CIRCLE':
                     // Uses state.fillAsset internally via getFillAssetPixel
                    this.drawing.fillCircle(p.X, p.Y, p.RADIUS, this.state);
                    break;
                 case 'DRAW': // Replaces old ICON drawing command
                     // p.NAME was passed directly by resolveParams (unquoted, uppercased identifier)
                     const patternNameUpper = p.NAME;
                     // Look up in the unified assets.assets store
                     const assetData = this.assets.assets[patternNameUpper];
                     if (!assetData) {
                         // Use command.line for error reporting
                         throw this.runtimeError(`Pattern "${patternNameUpper}" not defined (check DEFINE PATTERN).`, command.line);
                     }
                     // p.X, p.Y are resolved numbers
                     this.drawing.drawAsset(p.X, p.Y, assetData, this.state); // Use renamed drawing function
                     break;

                // --- Control Flow ---
                case 'REPEAT':
                    // Resolve count using current environment and command line number
                    // command.count is number or uppercase variable reference ($VAR) from parser
                    const count = this._resolveValue(command.count, currentEnv, command.line);
                    if (!Number.isInteger(count) || count < 0) {
                         // Use command.line for error reporting
                         throw this.runtimeError(`REPEAT COUNT must resolve to a non-negative integer. Got: ${count}`, command.line);
                    }
                    for (let i = 0; i < count; i++) {
                        // Execute nested commands, passing the loop index 'i'
                        for (const nestedCmd of command.commands) {
                            this.executeCommand(nestedCmd, i);
                        }
                    }
                    break;

                case 'IF':
                    // command.condition has uppercase variable references ($VAR) from parser
                    // Pass command line number for condition evaluation errors
                    const conditionMet = this.evaluateCondition(command.condition, currentEnv, command.line);
                    const blockToExecute = conditionMet ? command.thenCommands : command.elseCommands;

                    if (blockToExecute) {
                        for (const nestedCmd of blockToExecute) {
                            // Pass loop index if any (relevant if IF is inside REPEAT)
                            this.executeCommand(nestedCmd, loopIndex);
                        }
                    }
                    break;

                default:
                    // Should be caught by parser
                    throw this.runtimeError(`Unsupported command type encountered: ${command.type}`, command.line);
            }
        } catch (e) {
             // If it's already a runtime error, add line number and rethrow
             // Otherwise, wrap it in a runtime error with the current command's line number
             if (e.isRuntimeError) {
                 // Ensure line number is present if possible
                 if (!e.lineNumber && command && command.line) {
                     throw this.runtimeError(e.message.replace(/^Runtime Error(\s*\(Line \d+\))?:\s*/, ''), command.line);
                 }
                 throw e; // Already has line number info or couldn't add it
             } else {
                  // Add line number from the command that caused the unexpected error
                  throw this.runtimeError(e.message, command.line);
             }
        }
    }

     // Evaluate parsed expression tokens
     // Tokens have type 'num', 'op', or 'var' with value being number, operator string, or uppercase variable reference ($VARNAME)
     // lineNumber is for error reporting during value resolution.
     evaluateExpression(tokens, currentEnv, lineNumber) {
         // Resolve values first using the centralized resolver
         const resolvedTokens = tokens.map(token => {
             if (token.type === 'num') return token.value;
             if (token.type === 'var') {
                 // token.value is uppercase variable reference like $MYVAR
                 // Pass lineNumber for error reporting if variable is undefined
                 return this._resolveValue(token.value, currentEnv, lineNumber);
             }
             if (token.type === 'op') return token.value; // Keep operators as is
             throw this.runtimeError(`Invalid token type in expression: ${token.type}`, lineNumber);
         });

         // Basic Shunting-yard / Operator Precedence Logic (Simplified for limited operators)
         // We only have +, -, *, /, % with standard precedence. No parentheses.

         // Helper for applying an operation
         const applyOp = (val1, op, val2) => {
             // Ensure operands are numbers before operation
             if (typeof val1 !== 'number' || typeof val2 !== 'number') {
                 throw this.runtimeError(`Cannot perform operation '${op}' on non-numeric values: ${val1}, ${val2}`, lineNumber);
             }
             switch (op) {
                 case '+': return val1 + val2;
                 case '-': return val1 - val2;
                 case '*': return val1 * val2;
                 case '/':
                     if (val2 === 0) throw this.runtimeError(`Division by zero.`, lineNumber);
                     return Math.trunc(val1 / val2); // Integer division
                 case '%':
                     if (val2 === 0) throw this.runtimeError(`Modulo by zero.`, lineNumber);
                     // Ensure operands are integers for modulo, as JS % handles floats differently
                     if (!Number.isInteger(val1) || !Number.isInteger(val2)) {
                         throw this.runtimeError(`Modulo requires integer operands. Got: ${val1}, ${val2}`, lineNumber);
                     }
                     return val1 % val2;
                 default: throw this.runtimeError(`Unknown operator in expression: ${op}`, lineNumber);
             }
         };

         // Create copies for manipulation
         let values = resolvedTokens.filter(t => typeof t === 'number');
         let ops = resolvedTokens.filter(t => typeof t === 'string'); // Operators

         // Phase 1: Multiplication, Division, Modulo (left-to-right)
         let i = 0;
         while (i < ops.length) {
             if (ops[i] === '*' || ops[i] === '/' || ops[i] === '%') {
                 values[i] = applyOp(values[i], ops[i], values[i + 1]);
                 values.splice(i + 1, 1); // Remove right operand value
                 ops.splice(i, 1); // Remove operator
                 // Do not increment i, as the current index now holds the result
                 // and the next operator is now at index i.
             } else {
                 i++; // Move to next operator only if it wasn't high precedence
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
             // Should not happen if parsing was correct and evaluation logic is sound
             throw this.runtimeError(`Expression evaluation failed. Remaining values: ${values.length}`, lineNumber);
         }
         // Final result must be a number
         if (typeof values[0] !== 'number') {
              throw this.runtimeError(`Expression evaluation did not result in a number. Result: ${values[0]}`, lineNumber);
         }
         return values[0];
     }


     // Evaluate parsed condition object
     // Condition object has uppercase variable references ($VARNAME) from parser
     // lineNumber is for error reporting during value resolution.
     evaluateCondition(condition, currentEnv, lineNumber) {
         let leftVal, rightVal;

         // Resolve values using the central resolver
         // Pass the uppercase variable reference (e.g., $MYVAR) or number directly
         // Pass lineNumber for error reporting
         const resolve = (val) => this._resolveValue(val, currentEnv, lineNumber);

         try {
             if (condition.type === 'modulo') {
                 // leftVar is uppercase variable reference $VARNAME
                 const varValue = resolve(condition.leftVar);
                 const literal = condition.literal; // Literal is already a number from parser
                 // right is number or uppercase variable reference $VARNAME
                 const rightRaw = resolve(condition.right);

                 if (!Number.isInteger(varValue)) throw this.runtimeError(`Left side of modulo ($VAR) must resolve to an integer. Got: ${varValue}`, lineNumber);
                 if (!Number.isInteger(rightRaw)) throw this.runtimeError(`Right side of modulo comparison must resolve to an integer. Got: ${rightRaw}`, lineNumber);
                 if (!Number.isInteger(literal) || literal === 0) throw this.runtimeError(`Modulo literal must be a non-zero integer. Got: ${literal}`, lineNumber);

                 leftVal = varValue % literal;
                 rightVal = rightRaw;
             } else { // standard condition
                 // left/right are number or uppercase variable reference $VARNAME
                 leftVal = resolve(condition.left);
                 rightVal = resolve(condition.right);

                 // Ensure both sides resolved to numbers for comparison
                 if (typeof leftVal !== 'number' || typeof rightVal !== 'number') {
                      throw this.runtimeError(`Cannot compare non-numeric values in condition: ${leftVal} (type ${typeof leftVal}) vs ${rightVal} (type ${typeof rightVal})`, lineNumber);
                 }
             }

             switch (condition.operator) {
                 case '==': return leftVal === rightVal;
                 case '!=': return leftVal !== rightVal;
                 case '>': return leftVal > rightVal;
                 case '<': return leftVal < rightVal;
                 case '>=': return leftVal >= rightVal;
                 case '<=': return leftVal <= rightVal;
                 default: throw this.runtimeError(`Unknown operator in condition: ${condition.operator}`, lineNumber);
             }
         } catch (e) {
             // Re-throw errors caught during resolution or comparison, ensuring line number
             if (e.isRuntimeError) {
                 throw e; // Already has line number
             } else {
                 throw this.runtimeError(`Error evaluating condition: ${e.message}`, lineNumber);
             }
         }
     }

     // Helper to create runtime errors with line numbers
     runtimeError(message, lineNumber) {
         // Ensure lineNumber is attached if available and valid
         const lineInfo = (typeof lineNumber === 'number' && lineNumber > 0) ? ` (Line ${lineNumber})` : '';
         const error = new Error(`Runtime Error${lineInfo}: ${message}`);
         error.lineNumber = lineNumber;
         error.isRuntimeError = true; // Mark as runtime error
         return error;
     }
}