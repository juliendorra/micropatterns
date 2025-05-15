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
            
            // Pass additional optimization methods to the compiled function
            const optimizationHelpers = {
                getCachedTransform: this.getCachedTransform.bind(this),
                getCachedPatternTile: this.getCachedPatternTile.bind(this),
                batchPixelOperations: this.batchPixelOperations.bind(this),
                flushPixelBatch: this.flushPixelBatch.bind(this)
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
                optimizationHelpers // _optimization helpers
            );
            
            // Ensure any remaining batched operations are executed
            this.flushPixelBatch();
            
            // Optionally log optimization statistics
            if (compiledOutput.config && compiledOutput.config.logOptimizationStats) {
                let statsReport = "\n--- Optimization Stats ---\n";
                statsReport += `Transform Cache Hits: ${this.stats.transformCacheHits}\n`;
                statsReport += `Pattern Cache Hits: ${this.stats.patternCacheHits}\n`;
                statsReport += `Batched Operations: ${this.stats.batchedOperations}\n`;

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
        this.ctx.fillStyle = lastColor;
        
        // Use a single path for all pixels of the same color
        this.ctx.beginPath();
        for (const op of operations) {
            this.ctx.rect(Math.trunc(op.x), Math.trunc(op.y), 1, 1);
        }
        this.ctx.fill();
        
        // Reset the batch
        this.pixelBatchBuffer.operations = [];
    }
}