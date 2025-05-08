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
            scale: 1.0, // Current absolute scale factor
            matrix: new DOMMatrix(), // Current transformation matrix (translate, rotate)
            inverseMatrix: new DOMMatrix() // Inverse of state.matrix
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
             if (e.isRuntimeError) {
                 this.errorCallback(e.message);
             } else {
                 this.errorCallback(`Unexpected Runtime Error: ${e.message}`);
                 console.error("Unexpected Runtime Error Stack:", e.stack);
             }
        }
    }

     _resolveValue(valueSource, currentEnv, lineNumber) {
         if (typeof valueSource === 'number') {
             return valueSource;
         }
          if (typeof valueSource === 'string' && valueSource.startsWith('$')) {
              const varNameUpper = valueSource;
              if (varNameUpper === '$HOUR') return currentEnv.HOUR;
              if (varNameUpper === '$MINUTE') return currentEnv.MINUTE;
              if (varNameUpper === '$SECOND') return currentEnv.SECOND;
              if (varNameUpper === '$COUNTER') return currentEnv.COUNTER;
              if (varNameUpper === '$WIDTH') return currentEnv.WIDTH;
              if (varNameUpper === '$HEIGHT') return currentEnv.HEIGHT;
              if (varNameUpper === '$INDEX' && currentEnv.INDEX !== undefined) return currentEnv.INDEX;

              if (this.variables.hasOwnProperty(varNameUpper)) {
                  return this.variables[varNameUpper];
              } else {
                  throw this.runtimeError(`Undefined variable: ${valueSource}`, lineNumber);
              }
          } else {
              throw this.runtimeError(`Cannot resolve value: Expected number or variable ($VAR), got ${valueSource} (type: ${typeof valueSource})`, lineNumber);
          }
     }

     executeCommand(command, loopIndex = null) {
         const currentEnv = { ...this.environment };
         if (loopIndex !== null) {
             currentEnv.INDEX = loopIndex;
         }

         const resolveParams = (params, commandType, lineNumber) => {
             const resolved = {};
             const skipResolveKeys = (commandType === 'COLOR' || commandType === 'FILL' || commandType === 'DRAW') ? ['NAME'] : [];

             for (const key in params) {
                 if (skipResolveKeys.includes(key)) {
                     resolved[key] = params[key];
                 } else {
                     resolved[key] = this._resolveValue(params[key], currentEnv, lineNumber);
                 }
             }
             return resolved;
         };

        try {
            let p = {};
            const standardParamCommands = ['PIXEL', 'LINE', 'RECT', 'FILL_RECT', 'CIRCLE', 'FILL_CIRCLE', 'DRAW', 'TRANSLATE', 'ROTATE', 'SCALE', 'COLOR', 'FILL', 'RESET_TRANSFORMS', 'FILL_PIXEL'];
            if (command.params && standardParamCommands.includes(command.type)) {
                 p = resolveParams(command.params, command.type, command.line);
            }

            switch (command.type) {
                case 'NOOP':
                    break;
                 case 'VAR':
                     const varRef = '$' + command.varName;
                     if (command.initialExpression) {
                         const initialValue = this.evaluateExpression(command.initialExpression, currentEnv, command.line);
                         this.variables[varRef] = initialValue;
                     } else {
                         this.variables[varRef] = 0;
                     }
                     break;

                 case 'LET':
                     const value = this.evaluateExpression(command.expression, currentEnv, command.line);
                     this.variables['$' + command.targetVar] = value;
                     break;

                 case 'COLOR':
                     this.state.color = p.NAME.toLowerCase();
                     break;

                 case 'FILL':
                     if (p.NAME === 'SOLID') {
                         this.state.fillAsset = null;
                     } else {
                         const patternNameUpper = p.NAME;
                         const assetData = this.assets.assets[patternNameUpper];
                         if (!assetData) {
                             throw this.runtimeError(`Pattern "${patternNameUpper}" not defined.`, command.line);
                         }
                         this.state.fillAsset = assetData;
                     }
                     break;

                case 'RESET_TRANSFORMS':
                    this.state.scale = 1.0;
                    this.state.matrix = new DOMMatrix();
                    this.state.inverseMatrix = new DOMMatrix();
                    break;

                case 'TRANSLATE':
                    {
                        const dx = p.DX;
                        const dy = p.DY;
                        // M_new = M_current * T_op
                        this.state.matrix.translateSelf(dx, dy); // DOMMatrix.translateSelf post-multiplies
                        
                        // I_new = T_op_inv * I_current
                        const invTranslationMatrix = new DOMMatrix().translateSelf(-dx, -dy);
                        this.state.inverseMatrix = invTranslationMatrix.multiply(this.state.inverseMatrix);
                    }
                    break;

                case 'ROTATE':
                    {
                        const degrees = p.DEGREES;
                        // M_new = M_current * R_op
                        this.state.matrix.rotateSelf(degrees); // DOMMatrix.rotateSelf post-multiplies

                        // I_new = R_op_inv * I_current
                        const invRotationMatrix = new DOMMatrix().rotateSelf(-degrees);
                        this.state.inverseMatrix = invRotationMatrix.multiply(this.state.inverseMatrix);
                    }
                    break;

                case 'SCALE':
                    // SCALE command sets an absolute scale factor.
                    // This factor is applied *before* the matrix in drawing.js transformPoint.
                    // The matrix itself is not directly modified by the SCALE command.
                    this.state.scale = (p.FACTOR >= 1) ? p.FACTOR : 1.0;
                    break;

                case 'PIXEL':
                    this.drawing.drawPixel(p.X, p.Y, this.state);
                    break;
                case 'FILL_PIXEL':
                    this.drawing.drawFilledPixel(p.X, p.Y, this.state);
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
                 case 'DRAW':
                     const patternNameUpperDraw = p.NAME;
                     const assetDataDraw = this.assets.assets[patternNameUpperDraw];
                     if (!assetDataDraw) {
                         throw this.runtimeError(`Pattern "${patternNameUpperDraw}" not defined.`, command.line);
                     }
                     this.drawing.drawAsset(p.X, p.Y, assetDataDraw, this.state);
                     break;

                case 'REPEAT':
                    const count = this._resolveValue(command.count, currentEnv, command.line);
                    if (!Number.isInteger(count) || count < 0) {
                         throw this.runtimeError(`REPEAT COUNT must resolve to a non-negative integer. Got: ${count}`, command.line);
                    }
                    for (let i = 0; i < count; i++) {
                        for (const nestedCmd of command.commands) {
                            this.executeCommand(nestedCmd, i);
                        }
                    }
                    break;

                case 'IF':
                    const conditionMet = this.evaluateCondition(command.condition, currentEnv, command.line);
                    const blockToExecute = conditionMet ? command.thenCommands : command.elseCommands;

                    if (blockToExecute) {
                        for (const nestedCmd of blockToExecute) {
                            this.executeCommand(nestedCmd, loopIndex);
                        }
                    }
                    break;

                default:
                    throw this.runtimeError(`Unsupported command type: ${command.type}`, command.line);
            }
        } catch (e) {
             if (e.isRuntimeError) {
                 if (!e.lineNumber && command && command.line) {
                     throw this.runtimeError(e.message.replace(/^Runtime Error(\s*\(Line \d+\))?:\s*/, ''), command.line);
                 }
                 throw e;
             } else {
                  throw this.runtimeError(e.message, command.line);
             }
        }
    }

     evaluateExpression(tokens, currentEnv, lineNumber) {
         const resolvedTokens = tokens.map(token => {
             if (token.type === 'num') return token.value;
             if (token.type === 'var') {
                 return this._resolveValue(token.value, currentEnv, lineNumber);
             }
             if (token.type === 'op') return token.value;
             throw this.runtimeError(`Invalid token type in expression: ${token.type}`, lineNumber);
         });

         const applyOp = (val1, op, val2) => {
             if (typeof val1 !== 'number' || typeof val2 !== 'number') {
                 throw this.runtimeError(`Cannot perform operation '${op}' on non-numeric values: ${val1}, ${val2}`, lineNumber);
             }
             switch (op) {
                 case '+': return val1 + val2;
                 case '-': return val1 - val2;
                 case '*': return val1 * val2;
                 case '/':
                     if (val2 === 0) throw this.runtimeError(`Division by zero.`, lineNumber);
                     return Math.trunc(val1 / val2);
                 case '%':
                     if (val2 === 0) throw this.runtimeError(`Modulo by zero.`, lineNumber);
                     if (!Number.isInteger(val1) || !Number.isInteger(val2)) {
                         throw this.runtimeError(`Modulo requires integer operands. Got: ${val1}, ${val2}`, lineNumber);
                     }
                     return val1 % val2;
                 default: throw this.runtimeError(`Unknown operator: ${op}`, lineNumber);
             }
         };

         let values = resolvedTokens.filter(t => typeof t === 'number');
         let ops = resolvedTokens.filter(t => typeof t === 'string');

         let i = 0;
         while (i < ops.length) {
             if (ops[i] === '*' || ops[i] === '/' || ops[i] === '%') {
                 values[i] = applyOp(values[i], ops[i], values[i + 1]);
                 values.splice(i + 1, 1);
                 ops.splice(i, 1);
             } else {
                 i++;
             }
         }

         while (ops.length > 0) {
             values[0] = applyOp(values[0], ops[0], values[1]);
             values.splice(1, 1);
             ops.splice(0, 1);
         }

         if (values.length !== 1 || typeof values[0] !== 'number') {
             throw this.runtimeError(`Expression evaluation failed or did not result in a number. Result: ${values[0]}`, lineNumber);
         }
         return values[0];
     }

     evaluateCondition(condition, currentEnv, lineNumber) {
         let leftVal, rightVal;
         const resolve = (val) => this._resolveValue(val, currentEnv, lineNumber);

         try {
             if (condition.type === 'modulo') {
                 const varValue = resolve(condition.leftVar);
                 const literal = condition.literal;
                 const rightRaw = resolve(condition.right);

                 if (!Number.isInteger(varValue)) throw this.runtimeError(`Left side of modulo ($VAR) must be integer. Got: ${varValue}`, lineNumber);
                 if (!Number.isInteger(rightRaw)) throw this.runtimeError(`Right side of modulo comparison must be integer. Got: ${rightRaw}`, lineNumber);
                 if (!Number.isInteger(literal) || literal === 0) throw this.runtimeError(`Modulo literal must be non-zero integer. Got: ${literal}`, lineNumber);

                 leftVal = varValue % literal;
                 rightVal = rightRaw;
             } else {
                 leftVal = resolve(condition.left);
                 rightVal = resolve(condition.right);
                 if (typeof leftVal !== 'number' || typeof rightVal !== 'number') {
                      throw this.runtimeError(`Cannot compare non-numeric values: ${leftVal} vs ${rightVal}`, lineNumber);
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
             if (e.isRuntimeError) throw e;
             throw this.runtimeError(`Error evaluating condition: ${e.message}`, lineNumber);
         }
     }

     runtimeError(message, lineNumber) {
         const lineInfo = (typeof lineNumber === 'number' && lineNumber > 0) ? ` (Line ${lineNumber})` : '';
         const error = new Error(`Runtime Error${lineInfo}: ${message}`);
         error.lineNumber = lineNumber;
         error.isRuntimeError = true;
         return error;
     }
}