// micropatterns_emulator/compiler.js

export class MicroPatternsCompiler {
    constructor(optimizationConfig = {}) {
        this.errors = [];
        this.declaredVariables = new Set(); // Tracks variables declared with VAR ($VARNAME_UPPER)
        this.assetsDefinition = null; // Will be set during compile
        this.environmentDefinition = null; // Will be set during compile
        
        // Default optimization configuration
        this.optimizationConfig = {
            enableTransformCaching: true,       // Cache transformation matrices
            enablePatternTileCaching: true,     // Cache pattern tiles
            enablePixelBatching: true,          // Batch pixel operations
            enableLoopUnrolling: true,          // Unroll small loops
            loopUnrollThreshold: 8,             // Maximum loop count to unroll
            enableInvariantHoisting: true,      // Hoist invariant calculations
            enableFastPathSelection: true,      // Use specialized code paths for common cases
            enableSecondPassOptimization: true, // Enable second-pass optimizations
            enableDrawCallBatching: true,       // Batch similar drawing operations
            enableDeadCodeElimination: true,    // Remove code with no effect
            enableConstantFolding: true,        // Fold constant expressions
            enableTransformSequencing: true,    // Combine transformation sequences
            enableDrawOrderOptimization: true,  // Reorder operations to minimize state changes
            enableMemoryOptimization: true,     // Reduce memory allocations
            logOptimizationStats: false,        // Log optimization statistics
            ...optimizationConfig               // Override with user-provided config
        };
        
        // Statistics for second-pass optimizations
        this.secondPassStats = {
            drawCallsBatched: 0,
            transformsSequenced: 0,
            deadCodeEliminated: 0,
            constantsValuesSubstituted: 0,
            drawCallsReordered: 0,
            memoryAllocationsEliminated: 0
        };
    }

    _resetForCompilation(assetsDef, envDef) {
        this.errors = [];
        this.declaredVariables = new Set();
        this.assetsDefinition = assetsDef;
        this.environmentDefinition = envDef; // Store for context if needed by helpers
    }
    
    // Analyze the script to identify optimization opportunities
    _analyzeScriptForOptimizations(commands) {
        const analysis = {
            repeatLoops: [], // Track REPEAT loops for unrolling or invariant hoisting
            commonTransforms: new Set(), // Common transformation sequences
            commonPatternFills: new Set(), // Common pattern fills
            invariantExpressions: new Map(), // Expressions that don't change within loops
            conditionalPatterns: new Map(), // Common conditional patterns
        };
        
        // Helper for traversing nested commands
        const analyzeCommands = (cmds, context = { inLoop: false, loopVars: new Set() }) => {
            for (const cmd of cmds) {
                // Track REPEAT loops
                if (cmd.type === 'REPEAT') {
                    const loopInfo = {
                        line: cmd.line,
                        count: cmd.count,
                        canUnroll: this._canUnrollLoop(cmd),
                        invariants: this._findLoopInvariants(cmd)
                    };
                    analysis.repeatLoops.push(loopInfo);
                    
                    // Recursively analyze loop body with updated context
                    const loopContext = {
                        inLoop: true,
                        loopVars: new Set([...context.loopVars, '$INDEX'])
                    };
                    analyzeCommands(cmd.commands, loopContext);
                }
                // Track transformation sequences
                else if (['TRANSLATE', 'ROTATE', 'SCALE'].includes(cmd.type)) {
                    // Track common transform sequences that could be cached
                    if (context.currentTransformSeq) {
                        context.currentTransformSeq.push(cmd.type);
                    } else {
                        context.currentTransformSeq = [cmd.type];
                    }
                }
                // Track fill patterns
                else if (cmd.type === 'FILL') {
                    if (cmd.params && cmd.params.NAME !== 'SOLID') {
                        analysis.commonPatternFills.add(cmd.params.NAME);
                    }
                }
                // Track conditional patterns (IF/ELSE structures)
                else if (cmd.type === 'IF') {
                    // Recursively analyze IF/ELSE blocks
                    analyzeCommands(cmd.thenCommands, context);
                    if (cmd.elseCommands) {
                        analyzeCommands(cmd.elseCommands, context);
                    }
                    
                    // Track common conditional patterns for optimization
                    if (cmd.condition && cmd.condition.type === 'modulo' &&
                        typeof cmd.condition.literal === 'number') {
                        const key = `${cmd.condition.leftVar}%${cmd.condition.literal}${cmd.condition.operator}`;
                        if (!analysis.conditionalPatterns.has(key)) {
                            analysis.conditionalPatterns.set(key, 0);
                        }
                        analysis.conditionalPatterns.set(key, analysis.conditionalPatterns.get(key) + 1);
                    }
                }
                
                // If we hit a RESET_TRANSFORMS, clear the current transform sequence
                if (cmd.type === 'RESET_TRANSFORMS' && context.currentTransformSeq) {
                    delete context.currentTransformSeq;
                }
            }
        };
        
        analyzeCommands(commands);
        
        // Convert sets to arrays for easier serialization/inspection
        analysis.commonTransforms = Array.from(analysis.commonTransforms);
        analysis.commonPatternFills = Array.from(analysis.commonPatternFills);
        analysis.conditionalPatterns = Object.fromEntries(analysis.conditionalPatterns);
        
        return analysis;
    }
    
    // Determine if a loop can be safely unrolled
    _canUnrollLoop(loopCommand) {
        // Only unroll loops with constant counts below threshold
        if (typeof loopCommand.count === 'number' &&
            loopCommand.count <= this.optimizationConfig.loopUnrollThreshold) {
            return true;
        }
        
        // For variable counts, we need to determine if it's a constant expression
        // (This is a simplified approach - a full expression evaluator would be needed for all cases)
        return false;
    }
    
    // Find expressions that don't change inside a loop (invariants)
    _findLoopInvariants(loopCommand) {
        const invariants = [];
        const loopVarRefs = new Set(['$INDEX']); // Variables that change during the loop
        
        // Track expressions that don't use loop variables
        const checkExpressionInvariance = (tokens) => {
            if (!tokens) return false;
            
            // Check if any token references the loop variable
            for (const token of tokens) {
                if (token.type === 'var' && loopVarRefs.has(token.value)) {
                    return false;
                }
            }
            return true; // No loop variable references found
        };
        
        // Track commands in the loop body that define invariant calculations
        const findInvariantsInCommands = (commands) => {
            for (const cmd of commands) {
                if (cmd.type === 'LET') {
                    const isInvariant = checkExpressionInvariance(cmd.expression);
                    if (isInvariant) {
                        invariants.push({
                            type: 'expression',
                            target: cmd.targetVar,
                            expression: cmd.expression
                        });
                    } else {
                        // This variable is modified by a loop-dependent expression
                        loopVarRefs.add(`$${cmd.targetVar}`);
                    }
                }
                // Recursively check nested blocks
                else if (cmd.type === 'IF') {
                    findInvariantsInCommands(cmd.thenCommands);
                    if (cmd.elseCommands) {
                        findInvariantsInCommands(cmd.elseCommands);
                    }
                }
                else if (cmd.type === 'REPEAT') {
                    findInvariantsInCommands(cmd.commands);
                }
            }
        };
        
        findInvariantsInCommands(loopCommand.commands);
        return invariants;
    }
    
    // Extended script analysis for second-pass optimizations
    _extendScriptAnalysis(analysis, commands) {
        // Collect additional data for second-pass optimizations
        analysis.drawSequences = []; // Sequences of drawing operations
        analysis.transformSequences = []; // Sequences of transformation operations
        analysis.constantVariables = new Map(); // Variables that never change after initialization
        analysis.stateChanges = []; // Color and fill pattern changes
        analysis.commandsByType = {}; // Group commands by type for optimization
        
        // Analyze variable usage to find constants
        this._analyzeConstantVariables(commands, analysis.constantVariables);
        
        // Find drawing and transformation sequences
        this._analyzeOperationSequences(commands, analysis);
        
        // Count command types
        this._countCommandTypes(commands, analysis.commandsByType);
        
        return analysis;
    }
    
    // Analyze constant variables for optimization
    _analyzeConstantVariables(commands, constantVars) {
        // Track all variable assignments
        const assignments = new Map(); // variable -> count of assignments
        
        const trackAssignments = (cmds) => {
            for (const cmd of cmds) {
                if (cmd.type === 'VAR' || cmd.type === 'LET') {
                    const varName = cmd.type === 'VAR' ? cmd.varName : cmd.targetVar;
                    const count = assignments.get(varName) || 0;
                    assignments.set(varName, count + 1);
                }
                
                // Recursively check nested blocks
                if (cmd.type === 'IF') {
                    trackAssignments(cmd.thenCommands);
                    if (cmd.elseCommands) {
                        trackAssignments(cmd.elseCommands);
                    }
                }
                else if (cmd.type === 'REPEAT') {
                    trackAssignments(cmd.commands);
                }
            }
        };
        
        trackAssignments(commands);
        
        // Variables assigned exactly once are constants
        for (const [varName, count] of assignments.entries()) {
            if (count === 1) {
                constantVars.set(varName, null); // Will be filled with value during optimization
            }
        }
    }
    
    // Analyze operation sequences for batching
    _analyzeOperationSequences(commands, analysis) {
        const findSequences = (cmds, currentSequence = { draws: [], transforms: [] }, inLoop = false) => {
            for (const cmd of cmds) {
                // Track draw operations
                if (['PIXEL', 'LINE', 'RECT', 'FILL_RECT', 'CIRCLE', 'FILL_CIRCLE', 'DRAW', 'FILL_PIXEL'].includes(cmd.type)) {
                    currentSequence.draws.push({
                        type: cmd.type,
                        line: cmd.line,
                        inLoop: inLoop
                    });
                }
                
                // Track transformation operations
                if (['TRANSLATE', 'ROTATE', 'SCALE'].includes(cmd.type)) {
                    currentSequence.transforms.push({
                        type: cmd.type,
                        line: cmd.line,
                        inLoop: inLoop
                    });
                }
                
                // If we hit a RESET_TRANSFORMS, save the current sequence and start a new one
                if (cmd.type === 'RESET_TRANSFORMS' &&
                    (currentSequence.draws.length > 0 || currentSequence.transforms.length > 0)) {
                    
                    if (currentSequence.transforms.length > 1) {
                        analysis.transformSequences.push([...currentSequence.transforms]);
                    }
                    
                    if (currentSequence.draws.length > 1) {
                        analysis.drawSequences.push([...currentSequence.draws]);
                    }
                    
                    currentSequence.draws = [];
                    currentSequence.transforms = [];
                }
                
                // Track state changes
                if (cmd.type === 'COLOR' || cmd.type === 'FILL') {
                    analysis.stateChanges.push({
                        type: cmd.type,
                        line: cmd.line,
                        inLoop: inLoop,
                        params: cmd.params
                    });
                }
                
                // Recursively check nested blocks
                if (cmd.type === 'IF') {
                    // Clone the sequence for each branch
                    const thenSequence = {
                        draws: [...currentSequence.draws],
                        transforms: [...currentSequence.transforms]
                    };
                    const elseSequence = {
                        draws: [...currentSequence.draws],
                        transforms: [...currentSequence.transforms]
                    };
                    
                    findSequences(cmd.thenCommands, thenSequence, inLoop);
                    if (cmd.elseCommands) {
                        findSequences(cmd.elseCommands, elseSequence, inLoop);
                    }
                }
                else if (cmd.type === 'REPEAT') {
                    // For REPEAT, we note that operations are in a loop
                    findSequences(cmd.commands, currentSequence, true);
                }
            }
            
            // Save any remaining sequences
            if (currentSequence.transforms.length > 1) {
                analysis.transformSequences.push([...currentSequence.transforms]);
            }
            
            if (currentSequence.draws.length > 1) {
                analysis.drawSequences.push([...currentSequence.draws]);
            }
        };
        
        findSequences(commands);
    }
    
    // Count commands by type to prioritize optimizations
    _countCommandTypes(commands, commandCounts) {
        const countCommands = (cmds) => {
            for (const cmd of cmds) {
                commandCounts[cmd.type] = (commandCounts[cmd.type] || 0) + 1;
                
                // Recursively count nested blocks
                if (cmd.type === 'IF') {
                    countCommands(cmd.thenCommands);
                    if (cmd.elseCommands) {
                        countCommands(cmd.elseCommands);
                    }
                }
                else if (cmd.type === 'REPEAT') {
                    countCommands(cmd.commands);
                }
            }
        };
        
        countCommands(commands);
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

    _compileCommandToJs(command, scriptAnalysis = {}) {
        let js = `// Line ${command.line}: ${command.type}`;
        // Apply optimizations based on command type and script analysis
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
                    
                    if (this.optimizationConfig.enablePatternTileCaching) {
                        js += `\n// Use pattern tile caching`;
                        js += `\nconst _pattern_${command.line} = _assets['${patternNameUpper}'];`;
                        js += `\nif (!_pattern_${command.line}) { _runtimeError("Pattern '${patternNameUpper}' not defined.", ${command.line}); }`;
                        js += `\nelse {`;
                        js += `\n  const _patternKey_${command.line} = \`pattern:${patternNameUpper}:\${_state.color}\`;`;
                        js += `\n  _pattern_${command.line}.cachedTile = _optimization.getCachedPatternTile(_patternKey_${command.line}, () => {`;
                        js += `\n    // Pre-compute lookup table for this pattern`;
                        js += `\n    return {`;
                        js += `\n      lookup: new Uint8Array(_pattern_${command.line}.width * _pattern_${command.line}.height),`;
                        js += `\n      width: _pattern_${command.line}.width,`;
                        js += `\n      height: _pattern_${command.line}.height,`;
                        js += `\n      color: _state.color`;
                        js += `\n    };`;
                        js += `\n  });`;
                        js += `\n  _state.fillAsset = _pattern_${command.line};`;
                        js += `\n}`;
                    } else {
                        js += `\n_state.fillAsset = _assets['${patternNameUpper}'];`;
                        js += `\nif (!_state.fillAsset) { _runtimeError("Pattern '${patternNameUpper}' not defined.", ${command.line}); }`;
                    }
                }
                break;
            case 'RESET_TRANSFORMS':
                js += `\n_state.scale = 1.0; _state.matrix = new DOMMatrix(); _state.inverseMatrix = new DOMMatrix();`;
                break;
            case 'TRANSLATE':
                const dx = this._generateValueCode(command.params.DX, command.line);
                const dy = this._generateValueCode(command.params.DY, command.line);
                
                if (this.optimizationConfig.enableTransformCaching) {
                    js += `\n// Use transform caching for translation`;
                    js += `\nconst _dx_${command.line} = ${dx};`;
                    js += `\nconst _dy_${command.line} = ${dy};`;
                    js += `\nconst _transKey_${command.line} = \`translate:\${_dx_${command.line}}:\${_dy_${command.line}}:\${_state.matrix.toString()}\`;`;
                    js += `\nconst _transResult_${command.line} = _optimization.getCachedTransform(_transKey_${command.line}, () => {`;
                    js += `\n  const newMatrix = new DOMMatrix(_state.matrix);`;
                    js += `\n  newMatrix.translateSelf(_dx_${command.line}, _dy_${command.line});`;
                    js += `\n  return { matrix: newMatrix, inverse: newMatrix.inverse() };`;
                    js += `\n});`;
                    js += `\n_state.matrix = _transResult_${command.line}.matrix;`;
                    js += `\n_state.inverseMatrix = _transResult_${command.line}.inverse;`;
                } else {
                    js += `\n_state.matrix.translateSelf(${dx}, ${dy}); _state.inverseMatrix = _state.matrix.inverse();`;
                }
                break;
            case 'ROTATE':
                const degrees = this._generateValueCode(command.params.DEGREES, command.line);
                
                if (this.optimizationConfig.enableTransformCaching) {
                    js += `\n// Use transform caching for rotation`;
                    js += `\nconst _degrees_${command.line} = ${degrees};`;
                    js += `\nconst _rotKey_${command.line} = \`rotate:\${_degrees_${command.line}}:\${_state.matrix.toString()}\`;`;
                    js += `\nconst _rotResult_${command.line} = _optimization.getCachedTransform(_rotKey_${command.line}, () => {`;
                    js += `\n  const newMatrix = new DOMMatrix(_state.matrix);`;
                    js += `\n  newMatrix.rotateSelf(_degrees_${command.line});`;
                    js += `\n  return { matrix: newMatrix, inverse: newMatrix.inverse() };`;
                    js += `\n});`;
                    js += `\n_state.matrix = _rotResult_${command.line}.matrix;`;
                    js += `\n_state.inverseMatrix = _rotResult_${command.line}.inverse;`;
                } else {
                    js += `\n_state.matrix.rotateSelf(${degrees}); _state.inverseMatrix = _state.matrix.inverse();`;
                }
                break;
            case 'SCALE':
                const factor = this._generateValueCode(command.params.FACTOR, command.line);
                js += `\n_state.scale = (${factor} >= 1) ? ${factor} : 1.0;`;
                break;
            case 'PIXEL':
                if (this.optimizationConfig.enablePixelBatching) {
                    js += `\n// Use pixel batching`;
                    js += `\nconst _px_${command.line} = ${this._generateValueCode(command.params.X, command.line)};`;
                    js += `\nconst _py_${command.line} = ${this._generateValueCode(command.params.Y, command.line)};`;
                    js += `\nconst _transformedPt_${command.line} = _drawing.transformPoint(_px_${command.line}, _py_${command.line}, _state);`;
                    js += `\n_optimization.batchPixelOperations(_transformedPt_${command.line}.x, _transformedPt_${command.line}.y, _state.color);`;
                } else {
                    js += `\n_drawing.drawPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                }
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
                
                // Find loop invariants if optimization is enabled
                if (this.optimizationConfig.enableInvariantHoisting && scriptAnalysis.repeatLoops) {
                    const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                    if (loopInfo && loopInfo.invariants && loopInfo.invariants.length > 0) {
                        js += `\n// Hoist loop invariants`;
                        for (const inv of loopInfo.invariants) {
                            if (inv.type === 'expression') {
                                js += `\nconst _inv_${command.line}_${inv.target} = ${this._generateExpressionCode(inv.expression, command.line)};`;
                            }
                        }
                    }
                }
                
                // Check if this loop can be unrolled
                const canUnroll = this.optimizationConfig.enableLoopUnrolling &&
                                 scriptAnalysis.repeatLoops &&
                                 scriptAnalysis.repeatLoops.some(l => l.line === command.line && l.canUnroll);
                                 
                if (canUnroll && typeof command.count === 'number') {
                    // Unroll the loop for small constant counts
                    js += `\n// Unrolled loop (${command.count} iterations)`;
                    
                    for (let i = 0; i < command.count; i++) {
                        js += `\n// Iteration ${i}`;
                        js += `\n_variables['$INDEX'] = ${i};`;
                        
                        // Use invariants if available
                        if (this.optimizationConfig.enableInvariantHoisting) {
                            const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                            if (loopInfo && loopInfo.invariants) {
                                for (const inv of loopInfo.invariants) {
                                    if (inv.type === 'expression') {
                                        js += `\n_variables['$${inv.target}'] = _inv_${command.line}_${inv.target};`;
                                    }
                                }
                            }
                        }
                        
                        js += this._compileCommandsToString(command.commands, scriptAnalysis);
                    }
                } else {
                    // Standard loop implementation with optimization hooks
                    js += `\nfor (let _i_${command.line} = 0; _i_${command.line} < _repeatCount_${command.line}; _i_${command.line}++) {`;
                    js += `\n  // Set INDEX for this iteration`;
                    js += `\n  _variables['$INDEX'] = _i_${command.line};`;
                    
                    // Add runtime validation that $INDEX is properly set for each iteration
                    js += `\n  if (_variables['$INDEX'] === undefined) {`;
                    js += `\n    console.warn("INDEX variable undefined in REPEAT at line ${command.line}, iteration " + _i_${command.line});`;
                    js += `\n    _variables['$INDEX'] = _i_${command.line}; // Force correction`;
                    js += `\n  }`;
                    
                    // Use invariants if available
                    if (this.optimizationConfig.enableInvariantHoisting) {
                        const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                        if (loopInfo && loopInfo.invariants && loopInfo.invariants.length > 0) {
                            js += `\n  // Use pre-calculated invariants`;
                            for (const inv of loopInfo.invariants) {
                                if (inv.type === 'expression') {
                                    js += `\n  _variables['$${inv.target}'] = _inv_${command.line}_${inv.target};`;
                                }
                            }
                        }
                    }
                    
                    js += this._compileCommandsToString(command.commands, scriptAnalysis);
                    js += `\n}`;
                }
                
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
        let jsCodeLines = [];
        const initialUserVariables = {}; // For VAR declarations without initializers ($VAR_UPPER: 0)

        // First pass: Analyze the script for optimization opportunities
        const scriptAnalysis = this._analyzeScriptForOptimizations(parsedCommands);
        
        // Extended analysis for second-pass optimizations
        if (this.optimizationConfig.enableSecondPassOptimization) {
            this._extendScriptAnalysis(scriptAnalysis, parsedCommands);
        }
        
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
        console.log("Script analysis:", scriptAnalysis);
        
        // Second pass to generate code for all commands
        for (const command of parsedCommands) {
            try {
                // Log IF commands for debugging
                if (command.type === 'IF') {
                    console.log(`Processing IF command at line ${command.line}:`, command);
                }
                
                const commandJs = this._compileCommandToJs(command, scriptAnalysis);
                jsCodeLines.push(commandJs);
            } catch (e) {
                console.error(`Error compiling command at line ${command.line}:`, command, e);
                this.errors.push(`Compiler Error (Line ${command.line}): ${e.message}`);
                jsCodeLines.push(`\n_runtimeError("Compiler error: ${e.message}", ${command.line});`);
            }
        }
        
        // Apply second-pass optimizations if enabled
        if (this.optimizationConfig.enableSecondPassOptimization) {
            jsCodeLines = this._applySecondPassOptimizations(jsCodeLines, scriptAnalysis);
        }

        const functionBody = `
            // --- Compiled MicroPatterns Script ---
            // Arguments: _environment, _drawing, _assets, _initialUserVariables, _optimization
            
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
            
            // Performance tracking variables
            let _transformCacheHits = 0;
            let _patternLookupHits = 0;
            let _batchedDraws = 0;

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
            const compiledFunction = new Function('_environment', '_drawing', '_assets', '_initialUserVariables', '_optimization', functionBody);
            return {
                execute: compiledFunction,
                initialVariables: initialUserVariables, // Pass this to runner
                errors: this.errors,
                config: this.optimizationConfig, // Include optimization configuration
                secondPassStats: this.secondPassStats // Include second-pass optimization statistics
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
                errors: this.errors,
                config: this.optimizationConfig,
                secondPassStats: this.secondPassStats // Also include stats in error case
            };
        }
    }

    // Apply second-pass optimizations to the generated code
    _applySecondPassOptimizations(jsCodeLines, scriptAnalysis) {
        console.log("Applying second-pass optimizations...");
        
        // Reset optimization statistics
        this.secondPassStats = {
            drawCallsBatched: 0,
            transformsSequenced: 0,
            deadCodeEliminated: 0,
            constantsValuesSubstituted: 0,
            drawCallsReordered: 0,
            memoryAllocationsEliminated: 0
        };
        
        // Join code lines to process as a full string
        let code = jsCodeLines.join('\n');
        
        // Apply optimizations based on enabled features
        if (this.optimizationConfig.enableConstantFolding) {
            code = this._optimizeConstantExpressions(code, scriptAnalysis);
        }
        
        if (this.optimizationConfig.enableDeadCodeElimination) {
            code = this._eliminateDeadCode(code, scriptAnalysis);
        }
        
        if (this.optimizationConfig.enableTransformSequencing) {
            code = this._optimizeTransformSequences(code, scriptAnalysis);
        }
        
        if (this.optimizationConfig.enableDrawCallBatching) {
            code = this._batchDrawCalls(code, scriptAnalysis);
        }
        
        if (this.optimizationConfig.enableDrawOrderOptimization) {
            code = this._optimizeDrawOrder(code, scriptAnalysis);
        }
        
        if (this.optimizationConfig.enableMemoryOptimization) {
            code = this._optimizeMemoryUsage(code);
        }
        
        // Log optimization statistics if enabled
        if (this.optimizationConfig.logOptimizationStats) {
            console.log("Second-pass optimization stats:", this.secondPassStats);
        }
        
        // Split back into lines and return
        return code.split('\n');
    }
    
    // Optimize constant expressions by substituting known values
    _optimizeConstantExpressions(code, scriptAnalysis) {
        const constantVars = scriptAnalysis.constantVariables;
        if (!constantVars || constantVars.size === 0) return code;
        
        // First pass: find actual constant values
        const constRegex = /(_variables\['\$([A-Z0-9_]+)'\])\s*=\s*([^;]+);/g;
        let match;
        
        while ((match = constRegex.exec(code)) !== null) {
            const [, fullRef, varName, valueExpr] = match;
            
            if (constantVars.has(varName)) {
                // Simple heuristic: if valueExpr is a number, it's safe to substitute
                if (/^[0-9]+$/.test(valueExpr.trim())) {
                    constantVars.set(varName, valueExpr.trim());
                }
            }
        }
        
        // Second pass: substitute constant values
        for (const [varName, value] of constantVars.entries()) {
            if (value !== null) {
                const varRegex = new RegExp(`_variables\\['\\$${varName}'\\]`, 'g');
                let count = 0;
                code = code.replace(varRegex, (matchSub) => {
                    count++;
                    return value;
                });
                
                if (count > 0) { // Only add to stats if substitutions happened
                    this.secondPassStats.constantsValuesSubstituted += count -1; // -1 because the first assignment still exists
                }
            }
        }
        
        return code;
    }
    
    // Eliminate code with no effect (dead code)
    _eliminateDeadCode(code, scriptAnalysis) {
        // Identify transform operations with no drawing between them and RESET_TRANSFORMS
        let deadBlockRegex = /(\/\/ Line \d+: (TRANSLATE|ROTATE|SCALE)\n[\s\S]*?)(?=\n\s*\/\/ Line \d+: RESET_TRANSFORMS)/gs;
        
        code = code.replace(deadBlockRegex, (match, block) => {
            if (!/drawPixel|drawLine|drawRect|fillRect|drawCircle|fillCircle|drawAsset|drawFilledPixel/.test(block)) {
                this.secondPassStats.deadCodeEliminated++;
                return '// Dead code removed (transformation with no effect)';
            }
            return match;
        });
        
        // Remove unnecessary state changes (COLOR, FILL) that are immediately overwritten
        const colorRegex = /(\/\/ Line \d+: COLOR\n_state\.color = '[^']+';)(?=\s*\n\s*\/\/ Line \d+: COLOR)/g;
        code = code.replace(colorRegex, (match) => {
            this.secondPassStats.deadCodeEliminated++;
            return '// Dead code removed (redundant COLOR)';
        });
        
        const fillRegex = /(\/\/ Line \d+: FILL\n(?:_state\.fillAsset = null;_|_state\.fillAsset = _assets\['[^']+'\];.*?if \(!_state\.fillAsset\) \{ _runtimeError\("Pattern '[^']+' not defined\.", \d+\); \}))(?=\s*\n\s*\/\/ Line \d+: FILL)/gs;
        code = code.replace(fillRegex, (match) => {
            this.secondPassStats.deadCodeEliminated++;
            return '// Dead code removed (redundant FILL)';
        });
        
        return code;
    }
    
    // Optimize sequences of transformation operations
    _optimizeTransformSequences(code, scriptAnalysis) {
        const sequences = scriptAnalysis.transformSequences || [];
        
        for (const sequence of sequences) {
            if (sequence.length < 2 || sequence.some(op => op.inLoop)) continue;

            if (sequence.every(op => op.type === 'TRANSLATE')) {
                let firstLine = sequence[0].line;
                let lastLine = sequence[sequence.length - 1].line;

                // Construct a regex to match the block of TRANSLATE operations
                // This needs to match the code generated by _compileCommandToJs for TRANSLATE
                // when transform caching is enabled.
                let translateBlockPatternParts = [];
                for (const op of sequence) {
                    // This pattern matches the cached version of TRANSLATE
                    translateBlockPatternParts.push(
                        `// Line ${op.line}: TRANSLATE\\n` +
                        `const _dx_${op.line} = .*?;\\n` +
                        `const _dy_${op.line} = .*?;\\n` +
                        `const _transKey_${op.line} = \`translate:\${_dx_${op.line}}:\${_dy_${op.line}}:\${_state\\.matrix\\.toString\\(\\)}\`;\\n` +
                        `const _transResult_${op.line} = _optimization\\.getCachedTransform\\(_transKey_${op.line}, \\(\\) => \\{[\\s\\S]*?\\}\\);\\n` +
                        `_state\\.matrix = _transResult_${op.line}\\.matrix;\\n` +
                        `_state\\.inverseMatrix = _transResult_${op.line}\\.inverse;`
                    );
                }
                const translateBlockRegex = new RegExp(translateBlockPatternParts.join('\\n'), 's');
                
                code = code.replace(translateBlockRegex, (match) => {
                    this.secondPassStats.transformsSequenced += sequence.length -1; // N ops combined into 1
                    
                    let combinedDx = sequence.map(op => `_dx_${op.line}`).join(' + ');
                    let combinedDy = sequence.map(op => `_dy_${op.line}`).join(' + ');

                    return `// Combined ${sequence.length} TRANSLATE operations (Lines ${firstLine}-${lastLine})\\n` +
                           `const _combinedDX_${firstLine} = ${combinedDx};\\n` +
                           `const _combinedDY_${firstLine} = ${combinedDy};\\n` +
                           `_state.matrix.translateSelf(_combinedDX_${firstLine}, _combinedDY_${firstLine});\\n` + // Direct matrix operation
                           `_state.inverseMatrix = _state.matrix.inverse();`;
                });
            }
        }
        return code;
    }
    
    // Batch similar drawing operations to reduce state changes
    _batchDrawCalls(code, scriptAnalysis) {
        const sequences = scriptAnalysis.drawSequences || [];
        
        for (const sequence of sequences) {
            if (sequence.length < 2 || sequence.some(op => op.inLoop)) continue;
            
            if (sequence.every(op => op.type === 'PIXEL')) {
                let firstLine = sequence[0].line;
                let lastLine = sequence[sequence.length - 1].line;

                // This regex needs to match the code generated by _compileCommandToJs for PIXEL
                // when pixel batching is enabled.
                let pixelBlockPatternParts = [];
                for (const op of sequence) {
                     pixelBlockPatternParts.push(
                        `// Line ${op.line}: PIXEL\\n` +
                        `const _px_${op.line} = .*?;\\n` +
                        `const _py_${op.line} = .*?;\\n` +
                        `const _transformedPt_${op.line} = _drawing\\.transformPoint\\(_px_${op.line}, _py_${op.line}, _state\\);\\n` +
                        `_optimization\\.batchPixelOperations\\(_transformedPt_${op.line}\\.x, _transformedPt_${op.line}\\.y, _state\\.color\\);`
                     );
                }
                const pixelBlockRegex = new RegExp(pixelBlockPatternParts.join('\\n'), 's');
                
                code = code.replace(pixelBlockRegex, (match) => {
                    this.secondPassStats.drawCallsBatched += sequence.length;
                    
                    let pixelCoordDefs = sequence.map(op => `  { x: _px_${op.line}, y: _py_${op.line} }`).join(',\\n');
                    
                    return `// Batched ${sequence.length} PIXEL operations (Lines ${firstLine}-${lastLine})\\n` +
                           `const _batchedPixelCoords_${firstLine} = [\\n${pixelCoordDefs}\\n];\\n` +
                           `const _transformedBatchedPixels_${firstLine} = _batchedPixelCoords_${firstLine}.map(p => _drawing.transformPoint(p.x, p.y, _state));\\n` +
                           `_drawing.batchPixels(_transformedBatchedPixels_${firstLine}, _state.color, _state.scale);`;
                });
            }
        }
        return code;
    }
    
    // Reorder drawing operations to minimize state changes
    _optimizeDrawOrder(code, scriptAnalysis) {
        // This optimization is complex and can be risky.
        // A simple version might reorder COLOR commands if draws are identical.
        // Example: DRAW_A, COLOR_B, DRAW_A  -> COLOR_B, DRAW_A, DRAW_A
        // For now, returning code unchanged to avoid introducing subtle issues.
        return code;
    }
    
    // Optimize memory usage by reducing allocations
    _optimizeMemoryUsage(code) {
        // The matrix pool optimization is complex due to lifetime management.
        // Returning code unchanged for now.
        return code;
    }
}