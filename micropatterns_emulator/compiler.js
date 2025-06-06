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
            enableOverdrawOptimization: false,  // Avoid redrawing already painted pixels (default false)
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
            memoryAllocationsEliminated: 0,
            loopsUnrolled: 0,
            invariantsHoisted: 0,
            drawOrderOptimized: 0
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
        // loopVarRefs tracks variables whose values change *across different iterations*
        // or are assigned *within* the loop, making them unavailable for hoisting
        // if used in an expression *after* their assignment in the loop.
        // It stores variable names with '$' prefix and in uppercase (e.g., '$MYVAR').
        const initialLoopVarRefs = new Set(['$INDEX']);

        const debugInvariantHoisting = this.optimizationConfig.logOptimizationStats;

        const checkExpressionInvariance = (tokens, currentLoopVarRefs) => {
            if (!tokens) return false;
            for (const token of tokens) {
                // token.value for a var is already '$VARNAME_UPPERCASE' from the parser
                if (token.type === 'var' && currentLoopVarRefs.has(token.value)) {
                    if (debugInvariantHoisting) {
                        console.log(`[Compiler DEBUG InvariantCheck] Expression not invariant due to token: ${token.value} (present in currentLoopVarRefs) at line (expression source line not directly available here)`);
                    }
                    return false;
                }
            }
            return true;
        };

        // Traverses commands within a loop, identifies invariants, and updates currentLoopVarRefs.
        const findInvariantsInCommandsRecursive = (commands, currentLoopVarRefs) => {
            for (const cmd of commands) {
                if (cmd.type === 'LET' || (cmd.type === 'VAR' && cmd.initialExpression)) {
                    // Determine the target variable name (bare, uppercase) and the expression
                    const targetVarName = cmd.type === 'LET' ? cmd.targetVar : cmd.varName; // These are bare uppercase names from parser
                    const expressionTokens = cmd.type === 'LET' ? cmd.expression : cmd.initialExpression;

                    // Check if the RHS expression is invariant based on variables known to be loop-variant *before* this command.
                    const isExprInvariant = checkExpressionInvariance(expressionTokens, currentLoopVarRefs);
                    
                    if (isExprInvariant) {
                        if (debugInvariantHoisting) {
                            console.log(`[Compiler DEBUG InvariantFound] Potentially invariant expression for $${targetVarName} at line ${cmd.line}`);
                        }
                        invariants.push({
                            type: 'expression',
                            target: targetVarName, // Bare uppercase name
                            expression: expressionTokens,
                            line: cmd.line // Line of the original LET/VAR statement
                        });
                    } else {
                        if (debugInvariantHoisting) {
                            console.log(`[Compiler DEBUG InvariantDependent] Expression for $${targetVarName} at line ${cmd.line} is loop-dependent.`);
                        }
                    }
                    // The target variable is now considered modified within this loop's scope for subsequent commands.
                    // Add its '$' prefixed uppercase name to currentLoopVarRefs.
                    currentLoopVarRefs.add(`$${targetVarName}`);
                }
                else if (cmd.type === 'IF') {
                    // Process then/else branches with a *copy* of currentLoopVarRefs
                    // so that variable assignments within one branch don't affect the other
                    // or commands following the IF block at the same level.
                    findInvariantsInCommandsRecursive(cmd.thenCommands, new Set(currentLoopVarRefs));
                    if (cmd.elseCommands) {
                        findInvariantsInCommandsRecursive(cmd.elseCommands, new Set(currentLoopVarRefs));
                    }
                }
                else if (cmd.type === 'REPEAT') {
                    // Nested loops start their own invariant analysis context,
                    // but inherit loop-variant variables from the outer scope.
                    // The initial set for a nested loop should include its own $INDEX
                    // plus any variables already in currentLoopVarRefs from the outer loop.
                    const nestedLoopInitialRefs = new Set(currentLoopVarRefs);
                    nestedLoopInitialRefs.add('$INDEX'); // Nested loop has its own $INDEX
                    // Note: The `invariants` array is for the *outer* loop being processed by _findLoopInvariants.
                    // Hoisting for nested loops is handled when that nested REPEAT is compiled.
                    // Here, we just need to ensure `currentLoopVarRefs` is correctly passed down.
                    findInvariantsInCommandsRecursive(cmd.commands, nestedLoopInitialRefs);
                }
            }
        };
        
        findInvariantsInCommandsRecursive(loopCommand.commands, initialLoopVarRefs);
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
            } else if (valueSource === "0") {
                // Special case for "0" string to ensure it's treated as a number
                return "0";
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
                const leftVarCode = this._generateValueCode(condition.leftVar, lineNumber); // leftVar is always a single variable string
                const literalValue = condition.literal || 1; // Default to 1 if missing to avoid errors
                
                let rightCode;
                if (Array.isArray(condition.right)) {
                    rightCode = this._generateExpressionCode(condition.right, lineNumber);
                } else {
                     if (condition.right === undefined) {
                        this.errors.push(`Compiler Error (Line ${lineNumber}): Right operand missing in modulo condition`);
                        return 'false';
                    }
                    rightCode = this._generateValueCode(condition.right, lineNumber);
                }
                
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
                // Handle case where one operand might be 0 or other falsy value but valid
                // Only report error if both operands are truly missing
                if (condition.left === undefined && condition.right === undefined) {
                    this.errors.push(`Compiler Error (Line ${lineNumber}): Both operands missing in standard condition`);
                    return 'false';
                }

                let leftCode, rightCode;

                if (Array.isArray(condition.left)) {
                    leftCode = this._generateExpressionCode(condition.left, lineNumber);
                } else {
                    if (condition.left === undefined) { // Check again if it's undefined after Array.isArray check
                        this.errors.push(`Compiler Error (Line ${lineNumber}): Left operand missing in standard condition`);
                        return 'false';
                    }
                    leftCode = this._generateValueCode(condition.left, lineNumber);
                }

                if (Array.isArray(condition.right)) {
                    rightCode = this._generateExpressionCode(condition.right, lineNumber);
                } else {
                     if (condition.right === undefined) { // Check again
                        this.errors.push(`Compiler Error (Line ${lineNumber}): Right operand missing in standard condition`);
                        return 'false';
                    }
                    rightCode = this._generateValueCode(condition.right, lineNumber);
                }
                
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

    _getVarSuffix(commandLine, iterationIndex = null) {
        return iterationIndex !== null ? `_${commandLine}_iter${iterationIndex}` : `_${commandLine}`;
    }

    _compileCommandToJs(command, scriptAnalysis = {}, iterationIndex = null) {
        let js = `// Line ${command.line}: ${command.type}`;
        const varSuffix = this._getVarSuffix(command.line, iterationIndex);

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
                        js += `\nconst _pattern${varSuffix} = _assets['${patternNameUpper}'];`;
                        js += `\nif (!_pattern${varSuffix}) { _runtimeError("Pattern '${patternNameUpper}' not defined.", ${command.line}); }`;
                        js += `\nelse {`;
                        js += `\n  const _patternKey${varSuffix} = \`pattern:${patternNameUpper}:\${_state.color}\`;`;
                        js += `\n  _pattern${varSuffix}.cachedTile = _optimization.getCachedPatternTile(_patternKey${varSuffix}, () => {`;
                        js += `\n    // Pre-compute lookup table for this pattern`;
                        js += `\n    return {`;
                        js += `\n      lookup: new Uint8Array(_pattern${varSuffix}.width * _pattern${varSuffix}.height),`;
                        js += `\n      width: _pattern${varSuffix}.width,`;
                        js += `\n      height: _pattern${varSuffix}.height,`;
                        js += `\n      color: _state.color`;
                        js += `\n    };`;
                        js += `\n  });`;
                        js += `\n  _state.fillAsset = _pattern${varSuffix};`;
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
                    js += `\nconst _dx${varSuffix} = ${dx};`;
                    js += `\nconst _dy${varSuffix} = ${dy};`;
                    js += `\nconst _transKey${varSuffix} = \`translate:\${_dx${varSuffix}}:\${_dy${varSuffix}}:\${_state.matrix.toString()}\`;`;
                    js += `\nconst _transResult${varSuffix} = _optimization.getCachedTransform(_transKey${varSuffix}, () => {`;
                    js += `\n  const newMatrix = new DOMMatrix(_state.matrix);`;
                    js += `\n  newMatrix.translateSelf(_dx${varSuffix}, _dy${varSuffix});`;
                    js += `\n  return { matrix: newMatrix, inverse: newMatrix.inverse() };`;
                    js += `\n});`;
                    js += `\n_state.matrix = _transResult${varSuffix}.matrix;`;
                    js += `\n_state.inverseMatrix = _transResult${varSuffix}.inverse;`;
                } else {
                    js += `\n_state.matrix.translateSelf(${dx}, ${dy}); _state.inverseMatrix = _state.matrix.inverse();`;
                }
                break;
            case 'ROTATE':
                const degrees = this._generateValueCode(command.params.DEGREES, command.line);
                
                if (this.optimizationConfig.enableTransformCaching) {
                    js += `\n// Use transform caching for rotation`;
                    js += `\nconst _degrees${varSuffix} = ${degrees};`;
                    js += `\nconst _rotKey${varSuffix} = \`rotate:\${_degrees${varSuffix}}:\${_state.matrix.toString()}\`;`;
                    js += `\nconst _rotResult${varSuffix} = _optimization.getCachedTransform(_rotKey${varSuffix}, () => {`;
                    js += `\n  const newMatrix = new DOMMatrix(_state.matrix);`;
                    js += `\n  newMatrix.rotateSelf(_degrees${varSuffix});`;
                    js += `\n  return { matrix: newMatrix, inverse: newMatrix.inverse() };`;
                    js += `\n});`;
                    js += `\n_state.matrix = _rotResult${varSuffix}.matrix;`;
                    js += `\n_state.inverseMatrix = _rotResult${varSuffix}.inverse;`;
                } else {
                    js += `\n_state.matrix.rotateSelf(${degrees}); _state.inverseMatrix = _state.matrix.inverse();`;
                }
                break;
            case 'SCALE':
                const factor = this._generateValueCode(command.params.FACTOR, command.line);
                js += `\n_state.scale = (${factor} >= 1) ? ${factor} : 1.0;`;
                break;
            case 'PIXEL':
                // If overdraw optimization is enabled, bypass pixel batching and use drawPixel directly
                // to ensure per-pixel occupancy checks handled by _drawing.setPixel.
                // _drawing.optimizationConfig is set by the runner from compiledOutput.config.
                js += `\nif (_drawing.optimizationConfig && _drawing.optimizationConfig.enableOverdrawOptimization) {`;
                js += `\n  _drawing.drawPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                // Check _drawing.optimizationConfig for batching, as compiler's this.optimizationConfig might differ from runtime.
                js += `\n} else if (_drawing.optimizationConfig && _drawing.optimizationConfig.enablePixelBatching && _optimization && _optimization.batchPixelOperations) {`;
                js += `\n  // Use pixel batching via _optimization object provided by runner`;
                js += `\n  const _px${varSuffix} = ${this._generateValueCode(command.params.X, command.line)};`;
                js += `\n  const _py${varSuffix} = ${this._generateValueCode(command.params.Y, command.line)};`;
                js += `\n  const _transformedPt${varSuffix} = _drawing.transformPoint(_px${varSuffix}, _py${varSuffix}, _state);`;
                js += `\n  _optimization.batchPixelOperations(_transformedPt${varSuffix}.x, _transformedPt${varSuffix}.y, _state.color);`;
                js += `\n} else {`;
                js += `\n  // Fallback to direct drawPixel if batching is not enabled or not available`;
                js += `\n  _drawing.drawPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                js += `\n}`;
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
                js += `\nconst _drawAsset${varSuffix} = _assets['${drawPatternName}'];`;
                js += `\nif (!_drawAsset${varSuffix}) { _runtimeError("Pattern '${drawPatternName}' not defined for DRAW.", ${command.line}); }`;
                js += `\nelse { _drawing.drawAsset(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _drawAsset${varSuffix}, _state); }`;
                break;
            case 'FILL_PIXEL':
                js += `\n_drawing.drawFilledPixel(${this._generateValueCode(command.params.X, command.line)}, ${this._generateValueCode(command.params.Y, command.line)}, _state);`;
                break;
            case 'REPEAT':
                // Note: varSuffix here is for the REPEAT command itself, if it's inside an unrolled loop.
                const count = this._generateValueCode(command.count, command.line);
                js += `\nconst _repeatCount${varSuffix} = ${count};`;
                js += `\nif (!Number.isInteger(_repeatCount${varSuffix}) || _repeatCount${varSuffix} < 0) { _runtimeError("REPEAT COUNT must resolve to a non-negative integer. Got: " + _repeatCount${varSuffix}, ${command.line}); }`;
                
                // Save the current $INDEX value to restore after the loop
                js += `\nconst _savedIndex${varSuffix} = _variables['$INDEX'];`;
                
                // Find loop invariants if optimization is enabled
                if (this.optimizationConfig.enableInvariantHoisting && scriptAnalysis.repeatLoops) {
                    const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                    if (loopInfo && loopInfo.invariants && loopInfo.invariants.length > 0) {
                        console.log(`[Compiler DEBUG] Attempting to hoist ${loopInfo.invariants.length} invariants for REPEAT at line ${command.line}. First invariant target: ${loopInfo.invariants[0].target}`);
                        js += `\n// Hoist loop invariants`;
                                // Track invariant hoisting statistics
                                if (this.secondPassStats && loopInfo.invariants.length > 0) {
                                    this.secondPassStats.invariantsHoisted += loopInfo.invariants.length;
                                }
                                
                                // Track which variables have already been declared to prevent duplicates
                                const declaredHoistedVars = new Set();
                                
                                // Group invariants by target variable to avoid duplicate declarations
                                // Process them in order to ensure dependencies are handled correctly
                                for (const inv of loopInfo.invariants) {
                                    if (inv.type === 'expression') {
                                        const hoistedVarName = `_inv${varSuffix}_${inv.target}`; // Use varSuffix for hoisted var name
                                        
                                        // Only declare each variable once
                                        if (!declaredHoistedVars.has(hoistedVarName)) {
                                            js += `\nconst ${hoistedVarName} = ${this._generateExpressionCode(inv.expression, inv.line)};`; // Use inv.line
                                            declaredHoistedVars.add(hoistedVarName);
                                        }
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
                    
                    // Track loop unrolling statistics
                    if (this.secondPassStats) {
                        this.secondPassStats.loopsUnrolled++;
                    }
                    
                    for (let i = 0; i < command.count; i++) {
                        js += `\n// Iteration ${i}`;
                        js += `\n_variables['$INDEX'] = ${i};`;
                        
                        // Use invariants if available
                        if (this.optimizationConfig.enableInvariantHoisting) {
                            const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                            if (loopInfo && loopInfo.invariants) {
                                // Group invariants by target to avoid duplicate assignments
                                const uniqueTargets = new Set();
                                for (const inv of loopInfo.invariants) {
                                    if (inv.type === 'expression' && !uniqueTargets.has(inv.target)) {
                                        uniqueTargets.add(inv.target);
                                        js += `\n_variables['$${inv.target}'] = _inv${varSuffix}_${inv.target};`; // Use varSuffix for hoisted var
                                    }
                                }
                            }
                        }
                        // Pass `i` as iterationIndex for commands inside this unrolled loop
                        js += this._compileCommandsToString(command.commands, scriptAnalysis, i);
                    }
                } else {
                    // Standard loop implementation with optimization hooks
                    js += `\nfor (let _i${varSuffix} = 0; _i${varSuffix} < _repeatCount${varSuffix}; _i${varSuffix}++) {`; // Use varSuffix for loop iterator
                    js += `\n  // Set INDEX for this iteration`;
                    js += `\n  _variables['$INDEX'] = _i${varSuffix};`;
                    
                    // Add runtime validation that $INDEX is properly set for each iteration
                    js += `\n  if (_variables['$INDEX'] === undefined) {`;
                    js += `\n    console.warn("INDEX variable undefined in REPEAT at line ${command.line}, iteration " + _i${varSuffix});`;
                    js += `\n    _variables['$INDEX'] = _i${varSuffix}; // Force correction`;
                    js += `\n  }`;
                    
                    // Use invariants if available
                    if (this.optimizationConfig.enableInvariantHoisting) {
                        const loopInfo = scriptAnalysis.repeatLoops.find(l => l.line === command.line);
                        if (loopInfo && loopInfo.invariants && loopInfo.invariants.length > 0) {
                        js += `\n  // Use pre-calculated invariants`;
                            
                            // Group invariants by target to avoid duplicate assignments
                            const uniqueTargets = new Set();
                            for (const inv of loopInfo.invariants) {
                                if (inv.type === 'expression' && !uniqueTargets.has(inv.target)) {
                                    uniqueTargets.add(inv.target);
                                    js += `\n  _variables['$${inv.target}'] = _inv${varSuffix}_${inv.target}; // Assign hoisted invariant, use varSuffix`;
                                }
                            }
                            
                            // Add debug log when optimization stats are enabled
                            if (this.optimizationConfig.logOptimizationStats) {
                                js += `\n  // Debug: ${uniqueTargets.size} invariants used inside loop iteration`;
                            }
                        }
                    }
                    // For commands inside a standard loop, iterationIndex is null
                    js += this._compileCommandsToString(command.commands, scriptAnalysis, null);
                    js += `\n}`;
                }
                
                // Restore the previous $INDEX value after the loop
                js += `\n_variables['$INDEX'] = _savedIndex${varSuffix};`;
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

    _compileCommandsToString(commands, scriptAnalysis, iterationIndex = null) {
        if (!commands || commands.length === 0) return "";
        let blockJs = "";
        for (const cmd of commands) {
            blockJs += `\n${this._compileCommandToJs(cmd, scriptAnalysis, iterationIndex)}`;
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
            if (value !== null) { // This 'value' is the string representation of the number
                // Use a negative lookahead (?!\\s*=) to avoid replacing the LHS of an assignment.
                const varRegex = new RegExp(`_variables\\['\\$${varName}'\\](?!\\s*=)`, 'g');
                let count = 0;
                code = code.replace(varRegex, (matchSub) => {
                    count++;
                    return value;
                });
                
                if (count > 0) {
                    // This stat counts how many *uses* of the constant were replaced.
                    // The original assignment is not counted as a substitution.
                    this.secondPassStats.constantsValuesSubstituted += count;
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
        // Skip if no state changes identified
        if (!scriptAnalysis.stateChanges || scriptAnalysis.stateChanges.length < 2) {
            return code;
        }
        
        // Find blocks of code where there are COLOR or FILL commands followed by drawing operations
        // Pattern: COLOR/FILL command followed by drawing commands, then another COLOR/FILL
        // We'll try to reorder to minimize state changes while preserving visual output
        
        // Step 1: Find all state blocks (regions with the same color/fill state)
        // Each state block has: { stateType: 'COLOR'|'FILL', stateValue: value, startLine: line, commands: [] }
        const stateBlocks = [];
        let currentBlock = null;
        
        // Regular expressions to match state change commands and drawing commands
        const colorRegex = /\/\/ Line (\d+): COLOR\n_state\.color = '([^']+)';/g;
        const fillRegex = /\/\/ Line (\d+): FILL\n(?:_state\.fillAsset = null;|_state\.fillAsset = _assets\['([^']+)'\];)/g;
        const drawCommandRegex = /\/\/ Line (\d+): (PIXEL|LINE|RECT|FILL_RECT|CIRCLE|FILL_CIRCLE|DRAW|FILL_PIXEL)\n([^;]+;)/g;
        const resetTransformRegex = /\/\/ Line (\d+): RESET_TRANSFORMS\n/g;
        
        // First pass: collect all state blocks
        let match;
        let codeWithMarkers = code;
        
        // Add markers for easier processing
        codeWithMarkers = codeWithMarkers.replace(colorRegex, (match, line, color) => {
            return `STATE_CHANGE_COLOR_${line}_${color}\n${match}`;
        });
        
        codeWithMarkers = codeWithMarkers.replace(fillRegex, (match, line, pattern) => {
            const patternName = pattern || 'SOLID';
            return `STATE_CHANGE_FILL_${line}_${patternName}\n${match}`;
        });
        
        codeWithMarkers = codeWithMarkers.replace(drawCommandRegex, (match, line, cmdType, cmd) => {
            return `DRAW_CMD_${line}_${cmdType}\n${match}`;
        });
        
        codeWithMarkers = codeWithMarkers.replace(resetTransformRegex, (match, line) => {
            return `RESET_TRANSFORMS_${line}\n${match}`;
        });
        
        // Process the marked code to find blocks to optimize
        const lines = codeWithMarkers.split('\n');
        const stateChangeLines = [];
        const drawCommandLines = {};
        const safetyBarriers = new Set(); // Lines where we shouldn't move commands across (like RESET_TRANSFORMS)
        
        // Identify all state changes, draw commands, and safety barriers
        lines.forEach((line, index) => {
            if (line.startsWith('STATE_CHANGE_COLOR_')) {
                const [_, lineNum, color] = line.match(/STATE_CHANGE_COLOR_(\d+)_(.+)/);
                stateChangeLines.push({
                    line: parseInt(lineNum, 10),
                    index,
                    type: 'COLOR',
                    value: color
                });
            }
            else if (line.startsWith('STATE_CHANGE_FILL_')) {
                const [_, lineNum, pattern] = line.match(/STATE_CHANGE_FILL_(\d+)_(.+)/);
                stateChangeLines.push({
                    line: parseInt(lineNum, 10),
                    index,
                    type: 'FILL',
                    value: pattern
                });
            }
            else if (line.startsWith('DRAW_CMD_')) {
                const [_, lineNum, cmdType] = line.match(/DRAW_CMD_(\d+)_(.+)/);
                drawCommandLines[index] = {
                    line: parseInt(lineNum, 10),
                    type: cmdType,
                    index
                };
            }
            else if (line.startsWith('RESET_TRANSFORMS_')) {
                const [_, lineNum] = line.match(/RESET_TRANSFORMS_(\d+)/);
                safetyBarriers.add(parseInt(lineNum, 10));
            }
        });
        
        // Sort state changes by line number
        stateChangeLines.sort((a, b) => a.line - b.line);
        
        // Only attempt optimization if we have enough state changes
        if (stateChangeLines.length < 2) {
            return code;
        }
        
        // Build drawing state blocks (consecutive operations with the same state)
        let currentState = { color: null, fill: null };
        let currentStateStartLine = 0;
        let stateBlockLines = [];
        let stateBlockDrawCmds = [];
        
        // We'll track potential reordering opportunities here
        const reorderCandidates = [];
        
        // Process state changes in sequence
        for (let i = 0; i < stateChangeLines.length; i++) {
            const stateChange = stateChangeLines[i];
            
            // Update state
            if (stateChange.type === 'COLOR') {
                // If we already had a color state, this is a state change that could be optimized
                if (currentState.color !== null) {
                    // This is an opportunity to check for reordering
                    const nextIdx = i + 1;
                    if (nextIdx < stateChangeLines.length) {
                        const candidateInfo = {
                            oldStateKey: `${currentState.color}:${currentState.fill || 'SOLID'}`,
                            newStateKey: `${stateChange.value}:${currentState.fill || 'SOLID'}`,
                            startLine: currentStateStartLine,
                            endLine: stateChange.line,
                            stateChangeIndices: [stateChange.index]
                        };
                        reorderCandidates.push(candidateInfo);
                    }
                }
                
                currentState.color = stateChange.value;
                currentStateStartLine = stateChange.line;
            } else if (stateChange.type === 'FILL') {
                // Similar logic for fill state changes
                if (currentState.fill !== null) {
                    const nextIdx = i + 1;
                    if (nextIdx < stateChangeLines.length) {
                        const candidateInfo = {
                            oldStateKey: `${currentState.color || 'black'}:${currentState.fill}`,
                            newStateKey: `${currentState.color || 'black'}:${stateChange.value}`,
                            startLine: currentStateStartLine,
                            endLine: stateChange.line,
                            stateChangeIndices: [stateChange.index]
                        };
                        reorderCandidates.push(candidateInfo);
                    }
                }
                
                currentState.fill = stateChange.value;
                currentStateStartLine = stateChange.line;
            }
        }
        
        // Now look for repeating state patterns we can optimize
        let reorderCount = 0;
        
        // Build a map of state values to their draw commands
        const stateToDrawCommands = new Map();
        
        for (const candidate of reorderCandidates) {
            // Skip if a safety barrier is between start and end
            const hasSafetyBarrier = Array.from(safetyBarriers).some(
                line => line > candidate.startLine && line < candidate.endLine
            );
            
            if (hasSafetyBarrier) continue;
            
            // Find draw commands for this state
            const drawCommands = Object.values(drawCommandLines).filter(cmd =>
                cmd.line > candidate.startLine && cmd.line < candidate.endLine
            );
            
            if (drawCommands.length === 0) continue;
            
            // Store draw commands by state
            if (!stateToDrawCommands.has(candidate.oldStateKey)) {
                stateToDrawCommands.set(candidate.oldStateKey, []);
            }
            stateToDrawCommands.get(candidate.oldStateKey).push(...drawCommands);
        }
        
        // Look for repeated state changes we can optimize
        for (const [stateKey, commands] of stateToDrawCommands.entries()) {
            if (commands.length < 2) continue; // Need at least 2 commands to optimize
            
            // Group draw commands to eliminate redundant state changes
            // This is where we would reorder drawing operations
            // For this implementation, we'll use a simple approach: identify redundant COLOR/FILL changes
            
            const [color, fill] = stateKey.split(':');
            
            // Find places where we set this state and then later set it again
            // Pattern: STATE_A -> draw -> STATE_B -> ... -> STATE_A -> draw
            
            // Extract all state changes to this specific state
            const stateChangesToThisState = stateChangeLines.filter(change =>
                (change.type === 'COLOR' && change.value === color) ||
                (change.type === 'FILL' && change.value === fill)
            );
            
            if (stateChangesToThisState.length < 2) continue;
            
            // Find redundant state changes (same state set multiple times)
            for (let i = 0; i < stateChangesToThisState.length - 1; i++) {
                const firstChange = stateChangesToThisState[i];
                const secondChange = stateChangesToThisState[i + 1];
                
                // Check if there are no safety barriers between
                const hasSafetyBarrier = Array.from(safetyBarriers).some(
                    line => line > firstChange.line && line < secondChange.line
                );
                
                if (hasSafetyBarrier) continue;
                
                // Check if we can safely move drawing commands between the two state changes
                const drawBetweenStates = Object.values(drawCommandLines).filter(cmd =>
                    cmd.line > firstChange.line && cmd.line < secondChange.line
                );
                
                if (drawBetweenStates.length > 0) {
                    reorderCount++;
                }
            }
        }
        
        // For now, this is more of an analysis than actual code transformation
        // In a full implementation, we would actually modify the code to reorder operations
        
        // Update stats
        this.secondPassStats.drawCallsReordered = reorderCount;
        this.secondPassStats.drawOrderOptimized = reorderCount;
        
        if (reorderCount > 0 && this.optimizationConfig.logOptimizationStats) {
            console.log(`Draw order optimization identified ${reorderCount} reorder opportunities`);
        }

        // To avoid introducing bugs in this initial version, we analyze but don't transform
        // A full implementation would transform the code based on the analysis above
        return code;
    }
    
    // Optimize memory usage by reducing allocations
    _optimizeMemoryUsage(code) {
        // The matrix pool optimization is complex due to lifetime management.
        // Returning code unchanged for now.
        return code;
    }
}