// micropatterns_emulator/compiled_runtime.js
import { MicroPatternsDrawing } from './drawing.js';

export class MicroPatternsCompiledRunner {
    constructor(ctx, errorCallback, errorLogDiv) {
        this.ctx = ctx;
        this.errorLogDiv = errorLogDiv; // Store the error log div
        // Pass optimization methods and config to Drawing instance
        // Note: this.optimizationConfig is not yet set here, it comes from compiledOutput.config later.
        // We will pass compiledOutput.config to drawing methods that need it, or set it on drawing instance in execute.
        // For now, pass the methods that are bound and available.
        // The config part will be handled by passing it to drawing methods or setting it up in execute().
        // Let's defer passing optimizationConfig to drawing until execute, or pass it directly to methods.
        // Simpler: pass methods now, and drawing methods can check global config if needed, or we enhance later.
        // The most direct way for drawFilledPixel to access batchPixelOperations is if drawing instance has it.

        const boundBatchPixelOperations = this.batchPixelOperations.bind(this);
        // We need to ensure that the drawing instance has access to the config as well.
        // The config is available in `compiledOutput.config` within the `execute` method.
        // For now, we'll pass the batching function. The config check will be in drawing.js.
        // This implies drawing.js needs its own copy or access to the config.

        this.drawing = new MicroPatternsDrawing(
            ctx,
            { // Pass specific bound methods for batching
                batchPixelOperations: boundBatchPixelOperations,
                // flushPixelBatch: this.flushPixelBatch.bind(this) // Not strictly needed by drawing itself
            }
            // optimizationConfig will be passed or accessed differently.
            // Let's assume drawing will get config via state or direct param if needed.
        );
        this.errorCallback = errorCallback || ((msg) => { console.error("CompiledRunner Error:", msg); });
        
        // Caches for optimization
        this.transformCache = new Map(); // Cache for transformation matrices
        this.patternTileCache = new Map(); // Cache for pre-rendered pattern tiles
        this.lastDrawState = null; // Track last draw state to avoid redundant state changes
        this.pixelBatchBuffer = null; // Buffer for batched pixel operations
        this.stats = { // Performance statistics
            transformCacheHits: 0,
            patternCacheHits: 0,
            batchedOperations: 0
        };
    }

    resetDisplay() {
        this.ctx.fillStyle = 'white';
        this.ctx.fillRect(0, 0, this.drawing.display_width, this.drawing.display_height);
        
        // Reset caches on new execution
        this.transformCache.clear();
        this.patternTileCache.clear();
        this.lastDrawState = null;
        this.stats = {
            transformCacheHits: 0,
            patternCacheHits: 0,
            batchedOperations: 0
        };
    }

    execute(compiledOutput, assets, environment) {
        this.resetDisplay();
        try {
            if (typeof compiledOutput.execute !== 'function') {
                throw new Error("Compiled output does not contain an executable function.");
            }
            
            // Initialize performance timers if statistics are enabled
            const startTime = compiledOutput.config && compiledOutput.config.logOptimizationStats ?
                              performance.now() : null;
            
            // Pass additional optimization methods to the compiled function
            const optimizationHelpers = {
                getCachedTransform: this.getCachedTransform.bind(this),
                getCachedPatternTile: this.getCachedPatternTile.bind(this),
                batchPixelOperations: this.batchPixelOperations.bind(this),
                flushPixelBatch: this.flushPixelBatch.bind(this),
                // Add new optimization helpers for second-pass support
                precomputeTransforms: this.precomputeTransforms.bind(this),
                executeDrawBatch: this.executeDrawBatch.bind(this)
            };
            
            // Prepare execution state with expanded caching
            const executionState = {
                state: {
                    color: 'black',
                    fillAsset: null,
                    scale: 1.0,
                    matrix: new DOMMatrix(),
                    inverseMatrix: new DOMMatrix(),
                    // Add caches for second-pass optimization
                    transformCache: new Map(),
                    patternTileCache: new Map(),
                    drawBatches: new Map()
                }
            };
            
            // The compiledOutput.execute function will manage its own state
            
            // Provide the drawing instance with the current optimization configuration
            if (this.drawing && compiledOutput.config) {
                this.drawing.setOptimizationConfig(compiledOutput.config);
            }

            compiledOutput.execute(
                environment,      // _environment
                this.drawing,     // _drawing
                assets,           // _assets (assets.assets from parser)
                compiledOutput.initialVariables, // _initialUserVariables
                optimizationHelpers, // _optimization helpers
                executionState    // _executionState for sharing state across optimizations
            );
            
            // Ensure any remaining batched operations are executed
            this.flushPixelBatch();
            
            // Optionally log optimization statistics
            if (compiledOutput.config && compiledOutput.config.logOptimizationStats) {
                const endTime = performance.now();
                const executionTime = endTime - startTime;
                
                let statsReport = "\n--- Optimization Stats ---\n";

                // Add list of enabled optimizations
                statsReport += "--- Enabled Optimizations ---\n";
                const optimizationFlags = {
                    enableTransformCaching: "Transform Caching",
                    enablePatternTileCaching: "Pattern Tile Caching",
                    enablePixelBatching: "Pixel Batching",
                    enableLoopUnrolling: "Loop Unrolling",
                    enableInvariantHoisting: "Invariant Hoisting",
                    enableFastPathSelection: "Fast Path Selection",
                    enableSecondPassOptimization: "Second-Pass Optimization",
                    enableDrawCallBatching: "Draw Call Batching (Second Pass)",
                    enableDeadCodeElimination: "Dead Code Elimination (Second Pass)",
                        enableConstantFolding: "Constant Folding (Second Pass)",
                        enableTransformSequencing: "Transform Sequencing (Second Pass)",
                        enableDrawOrderOptimization: "Draw Order Optimization (Second Pass)",
                        enableMemoryOptimization: "Memory Optimization (Second Pass)"
                };

                let hasEnabledOptimizations = false;
                for (const key in compiledOutput.config) {
                    if (optimizationFlags[key] && compiledOutput.config[key] === true) {
                        statsReport += `${optimizationFlags[key]}: Enabled\n`;
                        hasEnabledOptimizations = true;
                    }
                }
                if (!hasEnabledOptimizations) {
                    statsReport += "None\n";
                }
                statsReport += "\n"; // Add a newline for separation

                statsReport += `Execution Time: ${executionTime.toFixed(2)}ms\n`;
                statsReport += `Transform Cache Hits: ${this.stats.transformCacheHits}\n`;
                statsReport += `Pattern Cache Hits: ${this.stats.patternCacheHits}\n`;
                statsReport += `Batched Operations: ${this.stats.batchedOperations}\n`;
                
                // Add second-pass statistics if available
                if (compiledOutput.secondPassStats) {
                    statsReport += "\n--- Second-Pass Optimization Stats ---\n";
                    for (const [key, value] of Object.entries(compiledOutput.secondPassStats)) {
                        // Make stat keys more readable
                        const readableKey = key.replace(/([A-Z])/g, ' $1').replace(/^./, str => str.toUpperCase());
                        statsReport += `${readableKey}: ${value}\n`;
                    }
                }

                console.log(statsReport); // Log to console as a formatted string

                if (this.errorLogDiv) { // Check if errorLogDiv is provided
                    this.errorLogDiv.textContent += statsReport; // Append to errorLog div
                }
            }
        } catch (e) {
            if (e.isCompiledRuntimeError) {
                this.errorCallback(e.message); // Error message already formatted by _runtimeError
            } else {
                // Catch unexpected JS errors from the compiled function itself
                this.errorCallback(`Unexpected Compiled Script Error: ${e.message}`);
                console.error("Unexpected Compiled Script Error Stack:", e.stack);
            }
        }
    }
    
    // Cache transformation matrices to avoid redundant calculations
    getCachedTransform(transformKey, computeTransformFn) {
        if (this.transformCache.has(transformKey)) {
            this.stats.transformCacheHits++;
            return this.transformCache.get(transformKey);
        }
        
        const transform = computeTransformFn();
        this.transformCache.set(transformKey, transform);
        return transform;
    }
    
    // Cache pre-rendered pattern tiles for faster filling
    getCachedPatternTile(patternKey, generateTileFn) {
        if (this.patternTileCache.has(patternKey)) {
            this.stats.patternCacheHits++;
            return this.patternTileCache.get(patternKey);
        }
        
        const patternTile = generateTileFn();
        this.patternTileCache.set(patternKey, patternTile);
        return patternTile;
    }
    
    // Batch pixel operations for more efficient rendering
    batchPixelOperations(x, y, color, operation = 'draw') {
        if (!this.pixelBatchBuffer) {
            this.pixelBatchBuffer = {
                operations: [],
                lastColor: null
            };
        }
        
        // If color changed, we need to flush the batch
        if (this.pixelBatchBuffer.lastColor !== color) {
            this.flushPixelBatch();
            this.pixelBatchBuffer.lastColor = color;
        }
        
        this.pixelBatchBuffer.operations.push({ x, y, operation });
        this.stats.batchedOperations++;
        
        // Flush if batch gets too large
        if (this.pixelBatchBuffer.operations.length >= 1000) {
            this.flushPixelBatch();
        }
    }
    
    // Execute batched pixel operations
    flushPixelBatch() {
        if (!this.pixelBatchBuffer || this.pixelBatchBuffer.operations.length === 0) {
            return;
        }
        
        const { operations, lastColor } = this.pixelBatchBuffer;
        
        // Optimization: For large batches (>1000 pixels), use ImageData for better performance
        if (operations.length > 1000) {
            this._executeBatchWithImageData(operations, lastColor);
        } else {
            this.ctx.fillStyle = lastColor;
            
            // Use a single path for all pixels of the same color
            this.ctx.beginPath();
            for (const op of operations) {
                this.ctx.rect(Math.trunc(op.x), Math.trunc(op.y), 1, 1);
            }
            this.ctx.fill();
        }
        
        // Reset the batch
        this.pixelBatchBuffer.operations = [];
    }
    
    // Optimized batch execution using ImageData
    _executeBatchWithImageData(operations, color) {
        // First pass: find bounds
        let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
        for (const op of operations) {
            const x = Math.trunc(op.x);
            const y = Math.trunc(op.y);
            minX = Math.min(minX, x);
            minY = Math.min(minY, y);
            maxX = Math.max(maxX, x);
            maxY = Math.max(maxY, y);
        }
        
        // Clip to canvas boundaries
        minX = Math.max(0, minX);
        minY = Math.max(0, minY);
        maxX = Math.min(this.drawing.display_width - 1, maxX);
        maxY = Math.min(this.drawing.display_height - 1, maxY);
        
        // Calculate dimensions
        const width = maxX - minX + 1;
        const height = maxY - minY + 1;
        
        // Skip if dimensions are invalid
        if (width <= 0 || height <= 0) return;
        
        // Prepare color values
        let r, g, b, a;
        if (color === 'black') {
            r = 0; g = 0; b = 0; a = 255;
        } else if (color === 'white') {
            r = 255; g = 255; b = 255; a = 255;
        } else {
            // Parse any other color format
            const tempCanvas = document.createElement('canvas');
            const tempCtx = tempCanvas.getContext('2d');
            tempCtx.fillStyle = color;
            r = parseInt(tempCtx.fillStyle.slice(1, 3), 16);
            g = parseInt(tempCtx.fillStyle.slice(3, 5), 16);
            b = parseInt(tempCtx.fillStyle.slice(5, 7), 16);
            a = 255;
        }
        
        // Create ImageData
        const imageData = this.ctx.createImageData(width, height);
        const data = imageData.data;
        
        // Initialize all pixels as transparent
        for (let i = 0; i < data.length; i += 4) {
            data[i + 3] = 0; // Alpha = 0 (transparent)
        }
        
        // Set only the pixels we need
        for (const op of operations) {
            const x = Math.trunc(op.x) - minX;
            const y = Math.trunc(op.y) - minY;
            
            // Skip out-of-bounds pixels
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            const index = (y * width + x) * 4;
            data[index] = r;
            data[index + 1] = g;
            data[index + 2] = b;
            data[index + 3] = a;
        }
        
        // Draw the ImageData
        this.ctx.putImageData(imageData, minX, minY);
    }
    
    // Precompute transformations for a sequence of operations
    precomputeTransforms(transformSequence, initialState) {
        const resultMatrix = new DOMMatrix();
        
        // Start with the initial matrix if provided
        if (initialState && initialState.matrix) {
            resultMatrix.a = initialState.matrix.a;
            resultMatrix.b = initialState.matrix.b;
            resultMatrix.c = initialState.matrix.c;
            resultMatrix.d = initialState.matrix.d;
            resultMatrix.e = initialState.matrix.e;
            resultMatrix.f = initialState.matrix.f;
        }
        
        // Apply each transformation in sequence
        for (const transform of transformSequence) {
            if (transform.type === 'translate') {
                resultMatrix.translateSelf(transform.dx, transform.dy);
            } else if (transform.type === 'rotate') {
                resultMatrix.rotateSelf(transform.degrees);
            } else if (transform.type === 'scale') {
                resultMatrix.scaleSelf(transform.factor, transform.factor);
            }
        }
        
        // Return the combined transformation matrix
        return {
            matrix: resultMatrix,
            inverse: resultMatrix.inverse()
        };
    }
    
    // Execute a batch of drawing operations with the same state
    executeDrawBatch(drawOperations, state) {
        if (!drawOperations || drawOperations.length === 0) return;
        
        // Group operations by type
        const operationsByType = {};
        for (const op of drawOperations) {
            if (!operationsByType[op.type]) {
                operationsByType[op.type] = [];
            }
            operationsByType[op.type].push(op);
        }
        
        // Handle each type of operation in batch
        for (const [type, ops] of Object.entries(operationsByType)) {
            if (type === 'PIXEL') {
                this._executePixelBatch(ops, state);
            }
            // Add more batch handlers for other types as needed
        }
        
        // Record statistics
        this.stats.batchedOperations += drawOperations.length;
    }
    
    // Execute a batch of PIXEL operations
    _executePixelBatch(pixelOperations, state) {
        // Extract coordinates
        const pixels = pixelOperations.map(op => ({
            x: op.x,
            y: op.y
        }));
        
        // Transform all pixels
        const transformedPixels = pixels.map(p => {
            const tp = this.drawing.transformPoint(p.x, p.y, state);
            return { x: tp.x, y: tp.y };
        });
        
        // Draw all pixels at once
        this.drawing.batchPixels(transformedPixels, state.color, state.scale);
    }
}