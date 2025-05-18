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
        
        // Detailed execution timing measurements for profiling
        this.executionStats = {
            totalTime: 0,
            resetTime: 0,
            compiledFunctionTime: 0,
            flushBatchTime: 0,
            drawingOperationsTime: 0,
            optimizationTime: 0,
            drawingOperationCounts: {
                pixel: 0,
                line: 0,
                rect: 0,
                fillRect: 0,
                circle: 0,
                fillCircle: 0,
                draw: 0,
                fillPixel: 0,
                transform: 0,
                total: 0
            }
        };
    }

    resetDisplay() {
        this.ctx.fillStyle = 'white';
        this.ctx.fillRect(0, 0, this.drawing.display_width, this.drawing.display_height);
        
        // Only call resetPixelOccupancyMap if it exists
        if (this.drawing && typeof this.drawing.resetPixelOccupancyMap === 'function') {
            this.drawing.resetPixelOccupancyMap(); // Reset for overdraw optimization
        }
        
        // Reset overdraw stats if the method exists
        if (this.drawing && typeof this.drawing.resetOverdrawStats === 'function') {
            this.drawing.resetOverdrawStats();
        }
        
        // Reset caches on new execution
        this.transformCache.clear();
        this.patternTileCache.clear();
        this.lastDrawState = null;
        this.stats = {
            transformCacheHits: 0,
            patternCacheHits: 0,
            batchedOperations: 0,
            overdrawSkippedPixels: 0
        };
    }

    execute(compiledOutput, assets, environment) {
        // Track overall execution time
        const totalStartTime = performance.now();
        
        // Track reset display time
        const resetStartTime = performance.now();
        this.resetDisplay();
        const resetEndTime = performance.now();
        this.executionStats.resetTime = resetEndTime - resetStartTime;
        
        try {
            if (typeof compiledOutput.execute !== 'function') {
                throw new Error("Compiled output does not contain an executable function.");
            }
            
            // Initialize performance timers
            const startTime = performance.now();
            
            // Track drawing operations - we'll wrap drawing methods to collect stats
            const originalDrawPixel = this.drawing.drawPixel;
            const originalDrawLine = this.drawing.drawLine;
            const originalDrawRect = this.drawing.drawRect;
            const originalFillRect = this.drawing.fillRect;
            const originalDrawCircle = this.drawing.drawCircle;
            const originalFillCircle = this.drawing.fillCircle;
            const originalDrawAsset = this.drawing.drawAsset;
            const originalDrawFilledPixel = this.drawing.drawFilledPixel;
            const originalTransformPoint = this.drawing.transformPoint;
            
            // Reset operation counts
            this.executionStats.drawingOperationCounts = {
                pixel: 0,
                line: 0,
                rect: 0,
                fillRect: 0,
                circle: 0,
                fillCircle: 0,
                draw: 0,
                fillPixel: 0,
                transform: 0,
                total: 0
            };
            
            // Wrap drawing methods to track counts and timing
            let drawingOperationsTime = 0;
            
            this.drawing.drawPixel = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawPixel.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.pixel++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.drawLine = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawLine.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.line++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.drawRect = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawRect.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.rect++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.fillRect = (...args) => {
                const drawStartTime = performance.now();
                const result = originalFillRect.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.fillRect++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.drawCircle = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawCircle.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.circle++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.fillCircle = (...args) => {
                const drawStartTime = performance.now();
                const result = originalFillCircle.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.fillCircle++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.drawAsset = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawAsset.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.draw++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.drawFilledPixel = (...args) => {
                const drawStartTime = performance.now();
                const result = originalDrawFilledPixel.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.fillPixel++;
                this.executionStats.drawingOperationCounts.total++;
                return result;
            };
            
            this.drawing.transformPoint = (...args) => {
                const drawStartTime = performance.now();
                const result = originalTransformPoint.apply(this.drawing, args);
                drawingOperationsTime += performance.now() - drawStartTime;
                this.executionStats.drawingOperationCounts.transform++;
                return result;
            };
            
            // Track optimization operations time
            let optimizationTime = 0;
            
            // Create wrapped optimization helpers that track timing
            const originalOptimizationHelpers = {
                getCachedTransform: this.getCachedTransform.bind(this),
                getCachedPatternTile: this.getCachedPatternTile.bind(this),
                batchPixelOperations: this.batchPixelOperations.bind(this),
                flushPixelBatch: this.flushPixelBatch.bind(this),
                precomputeTransforms: this.precomputeTransforms.bind(this),
                executeDrawBatch: this.executeDrawBatch.bind(this)
            };
            
            const optimizationHelpers = {};
            
            // Wrap each optimization method to track timing
            for (const [methodName, method] of Object.entries(originalOptimizationHelpers)) {
                optimizationHelpers[methodName] = (...args) => {
                    const optStartTime = performance.now();
                    const result = method(...args);
                    optimizationTime += performance.now() - optStartTime;
                    return result;
                };
            }
            
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
            
            // Provide the drawing instance with the current optimization configuration
            if (this.drawing && compiledOutput.config) {
                this.drawing.setOptimizationConfig(compiledOutput.config);
            }

            // Execute the compiled function and track its execution time
            const execStartTime = performance.now();
            compiledOutput.execute(
                environment,      // _environment
                this.drawing,     // _drawing
                assets,           // _assets (assets.assets from parser)
                compiledOutput.initialVariables, // _initialUserVariables
                optimizationHelpers, // _optimization helpers (wrapped for timing)
                executionState    // _executionState for sharing state across optimizations
            );
            const execEndTime = performance.now();
            this.executionStats.compiledFunctionTime = execEndTime - execStartTime - drawingOperationsTime - optimizationTime;
            
            // Restore original drawing methods
            this.drawing.drawPixel = originalDrawPixel;
            this.drawing.drawLine = originalDrawLine;
            this.drawing.drawRect = originalDrawRect;
            this.drawing.fillRect = originalFillRect;
            this.drawing.drawCircle = originalDrawCircle;
            this.drawing.fillCircle = originalFillCircle;
            this.drawing.drawAsset = originalDrawAsset;
            this.drawing.drawFilledPixel = originalDrawFilledPixel;
            this.drawing.transformPoint = originalTransformPoint;
            
            // Track time for final flush
            const flushStartTime = performance.now();
            this.flushPixelBatch();
            const flushEndTime = performance.now();
            this.executionStats.flushBatchTime = flushEndTime - flushStartTime;
            
            // Store the accumulated drawing and optimization times
            this.executionStats.drawingOperationsTime = drawingOperationsTime;
            this.executionStats.optimizationTime = optimizationTime;
            
            // Calculate and store total execution time
            const totalEndTime = performance.now();
            this.executionStats.totalTime = totalEndTime - totalStartTime;
            
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
                    enableMemoryOptimization: "Memory Optimization (Second Pass)",
                    enableOverdrawOptimization: "Overdraw Optimization (Pixel Occupancy)" // Already present from previous thought, ensure it's correct
                };

                const secondPassSubOptionKeys = new Set([
                    "enableDrawCallBatching",
                    "enableDeadCodeElimination",
                    "enableConstantFolding",
                    "enableTransformSequencing",
                    "enableDrawOrderOptimization",
                    "enableMemoryOptimization"
                ]);

                const isMasterSecondPassEnabled = compiledOutput.config.enableSecondPassOptimization === true;
                let hasEnabledOptimizations = false;

                for (const key in optimizationFlags) { // Iterate over defined flags to maintain order
                    if (compiledOutput.config.hasOwnProperty(key)) {
                        let effectivelyEnabled = compiledOutput.config[key] === true;

                        // If this key is for a second-pass sub-option AND the master second-pass is disabled,
                        // then this sub-option is effectively disabled for logging.
                        if (secondPassSubOptionKeys.has(key) && !isMasterSecondPassEnabled) {
                            effectivelyEnabled = false;
                        }

                        if (effectivelyEnabled) {
                            statsReport += `${optimizationFlags[key]}: Enabled\n`;
                            hasEnabledOptimizations = true;
                        }
                        // To explicitly log "Disabled" status for all flags:
                        // else {
                        //     statsReport += `${optimizationFlags[key]}: Disabled\n`;
                        // }
                    }
                }

                if (!hasEnabledOptimizations) {
                    statsReport += "None\n";
                }
                statsReport += "\n"; // Add a newline for separation

                // Add the detailed execution timing statistics
                statsReport += `Execution Time: ${executionTime.toFixed(2)}ms\n`;
                statsReport += `Transform Cache Hits: ${this.stats.transformCacheHits}\n`;
                statsReport += `Pattern Cache Hits: ${this.stats.patternCacheHits}\n`;
                statsReport += `Batched Operations: ${this.stats.batchedOperations}\n`;
                
                // Add overdraw optimization stats if available
                if (this.drawing && typeof this.drawing.getOverdrawStats === 'function') {
                    const overdrawStats = this.drawing.getOverdrawStats();
                    this.stats.overdrawSkippedPixels = overdrawStats.skippedPixels;
                    statsReport += `Overdraw Skipped Pixels: ${this.stats.overdrawSkippedPixels}\n`;
                }
                
                // Add detailed execution phase breakdown
                statsReport += "\n--- Execution Timing Breakdown ---\n";
                statsReport += `Total Execution: ${this.executionStats.totalTime.toFixed(2)}ms\n`;
                statsReport += `Display Reset: ${this.executionStats.resetTime.toFixed(2)}ms (${(this.executionStats.resetTime / this.executionStats.totalTime * 100).toFixed(1)}%)\n`;
                statsReport += `Script Execution: ${this.executionStats.compiledFunctionTime.toFixed(2)}ms (${(this.executionStats.compiledFunctionTime / this.executionStats.totalTime * 100).toFixed(1)}%)\n`;
                statsReport += `Drawing Operations: ${this.executionStats.drawingOperationsTime.toFixed(2)}ms (${(this.executionStats.drawingOperationsTime / this.executionStats.totalTime * 100).toFixed(1)}%)\n`;
                statsReport += `Optimization Operations: ${this.executionStats.optimizationTime.toFixed(2)}ms (${(this.executionStats.optimizationTime / this.executionStats.totalTime * 100).toFixed(1)}%)\n`;
                statsReport += `Final Batch Flush: ${this.executionStats.flushBatchTime.toFixed(2)}ms (${(this.executionStats.flushBatchTime / this.executionStats.totalTime * 100).toFixed(1)}%)\n`;
                
                // Add drawing operation counts
                statsReport += "\n--- Drawing Operation Counts ---\n";
                statsReport += `Total Drawing Operations: ${this.executionStats.drawingOperationCounts.total}\n`;
                statsReport += `Pixels: ${this.executionStats.drawingOperationCounts.pixel}\n`;
                statsReport += `Lines: ${this.executionStats.drawingOperationCounts.line}\n`;
                statsReport += `Rectangles: ${this.executionStats.drawingOperationCounts.rect}\n`;
                statsReport += `Filled Rectangles: ${this.executionStats.drawingOperationCounts.fillRect}\n`;
                statsReport += `Circles: ${this.executionStats.drawingOperationCounts.circle}\n`;
                statsReport += `Filled Circles: ${this.executionStats.drawingOperationCounts.fillCircle}\n`;
                statsReport += `Pattern Draws: ${this.executionStats.drawingOperationCounts.draw}\n`;
                statsReport += `Filled Pixels: ${this.executionStats.drawingOperationCounts.fillPixel}\n`;
                statsReport += `Transformations: ${this.executionStats.drawingOperationCounts.transform}\n`;
                
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

        // Determine if we should use ImageData (which handles overdraw internally if enabled)
        // or the path/fill method (which needs explicit overdraw handling here if enabled).
        const useImageData = operations.length > 1000; // Threshold for using ImageData

        if (useImageData) {
            // _executeBatchWithImageData already handles overdraw optimization if enabled via this.drawing.optimizationConfig.
            this._executeBatchWithImageData(operations, lastColor);
        } else {
            // Use path/fill method for smaller batches.
            this.ctx.fillStyle = lastColor;
            
            if (this.drawing.optimizationConfig && this.drawing.optimizationConfig.enableOverdrawOptimization && this.drawing.pixelOccupancyMap) {
                // Overdraw is ON, and not using ImageData path.
                
                // Sort operations by y-coordinate for better row-based processing
                operations.sort((a, b) => {
                    const yDiff = Math.trunc(a.y) - Math.trunc(b.y);
                    return yDiff !== 0 ? yDiff : Math.trunc(a.x) - Math.trunc(b.x);
                });
                
                // Process operations in rows for better efficiency
                let currentY = null;
                let rowSpans = [];
                let currentSpan = null;
                
                for (let i = 0; i < operations.length; i++) {
                    const op = operations[i];
                    const sx = Math.trunc(op.x);
                    const sy = Math.trunc(op.y);
                    
                    // Skip out-of-bounds pixels
                    if (sx < 0 || sx >= this.drawing.display_width || sy < 0 || sy >= this.drawing.display_height) {
                        continue;
                    }
                    
                    // Check if we're starting a new row
                    if (currentY !== sy) {
                        // Draw any pending spans from the previous row
                        this._drawRowSpans(rowSpans);
                        rowSpans = [];
                        currentSpan = null;
                        currentY = sy;
                    }
                    
                    // Check occupancy map
                    const mapIndex = sy * this.drawing.display_width + sx;
                    if (this.drawing.pixelOccupancyMap[mapIndex] === 0) {
                        // This pixel is not occupied
                        
                        // If we have an active span and this pixel is adjacent, extend it
                        if (currentSpan && sx === currentSpan.endX + 1) {
                            currentSpan.endX = sx;
                        } else {
                            // Start a new span
                            if (currentSpan) {
                                rowSpans.push(currentSpan);
                            }
                            currentSpan = { startX: sx, endX: sx, y: sy };
                        }
                        
                        // Mark as occupied
                        this.drawing.pixelOccupancyMap[mapIndex] = 1;
                    } else {
                        // This pixel is already occupied
                        
                        // Close any active span
                        if (currentSpan) {
                            rowSpans.push(currentSpan);
                            currentSpan = null;
                        }
                        
                        this.drawing.overdrawSkippedPixels++;
                    }
                }
                
                // Draw any remaining spans
                if (currentSpan) {
                    rowSpans.push(currentSpan);
                }
                this._drawRowSpans(rowSpans);
                
            } else {
                // Overdraw is OFF, or not using occupancy map. Use standard batch fill.
                this.ctx.beginPath();
                for (const op of operations) {
                    this.ctx.rect(Math.trunc(op.x), Math.trunc(op.y), 1, 1);
                }
                this.ctx.fill();
            }
        }
        
        // Reset the batch
        this.pixelBatchBuffer.operations = [];
    }
    
    // Helper method to draw row spans efficiently
    _drawRowSpans(rowSpans) {
        if (rowSpans.length === 0) return;
        
        // For very small number of spans, just draw them individually
        if (rowSpans.length <= 3) {
            for (const span of rowSpans) {
                this.ctx.fillRect(
                    span.startX,
                    span.y,
                    span.endX - span.startX + 1,
                    1
                );
            }
            return;
        }
        
        // For larger number of spans, use path/fill for better performance
        this.ctx.beginPath();
        for (const span of rowSpans) {
            this.ctx.rect(
                span.startX,
                span.y,
                span.endX - span.startX + 1,
                1
            );
        }
        this.ctx.fill();
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
            const screenX = Math.trunc(op.x);
            const screenY = Math.trunc(op.y);

            // Relative coordinates for imageData
            const x_rel = screenX - minX;
            const y_rel = screenY - minY;
            
            // Skip out-of-bounds pixels for imageData
            if (x_rel < 0 || x_rel >= width || y_rel < 0 || y_rel >= height) continue;
            
            const dataIndex = (y_rel * width + x_rel) * 4;

            if (this.drawing.optimizationConfig && this.drawing.optimizationConfig.enableOverdrawOptimization && this.drawing.pixelOccupancyMap) {
                // Absolute screen coordinates for occupancy map
                if (screenX >= 0 && screenX < this.drawing.display_width && screenY >= 0 && screenY < this.drawing.display_height) {
                    const occupancyMapIndex = screenY * this.drawing.display_width + screenX;
                    const existingValue = this.drawing.pixelOccupancyMap[occupancyMapIndex];
                    const newValue = color === 'black' ? 1 : 2; // Match _colorToOccupancyValue logic
                    
                    if (existingValue === 0 || existingValue !== newValue) {
                        // Pixel is unoccupied or different color - draw it
                        data[dataIndex] = r;
                        data[dataIndex + 1] = g;
                        data[dataIndex + 2] = b;
                        data[dataIndex + 3] = a;
                        this.drawing.pixelOccupancyMap[occupancyMapIndex] = newValue;
                    } else {
                        // Already painted with same color, make transparent
                        data[dataIndex + 3] = 0;
                        this.drawing.overdrawSkippedPixels++; // Count skipped pixel
                    }
                } else {
                     // Outside canvas bounds for occupancy map, make transparent in imageData
                    data[dataIndex + 3] = 0;
                }
            } else {
                // Original logic without overdraw optimization
                data[dataIndex] = r;
                data[dataIndex + 1] = g;
                data[dataIndex + 2] = b;
                data[dataIndex + 3] = a;
            }
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