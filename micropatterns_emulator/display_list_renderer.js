// micropatterns_emulator/display_list_renderer.js
import { MicroPatternsDrawing } from './drawing.js';

class OcclusionBuffer {
    constructor(canvasWidth, canvasHeight, blockSize) {
        this.canvasWidth = canvasWidth;
        this.canvasHeight = canvasHeight;
        this.blockSize = blockSize || 16; // Default block size
        this.gridWidth = Math.ceil(canvasWidth / this.blockSize);
        this.gridHeight = Math.ceil(canvasHeight / this.blockSize);
        this.grid = new Uint8Array(this.gridWidth * this.gridHeight); // 0 = empty, 1 = opaque
        this.culledByOcclusionCount = 0;
    }

    reset() {
        this.grid.fill(0);
        this.culledByOcclusionCount = 0;
    }

    _getGridIndices(screenMinX, screenMinY, screenMaxX, screenMaxY) {
        const startCol = Math.max(0, Math.floor(screenMinX / this.blockSize));
        const endCol = Math.min(this.gridWidth - 1, Math.floor(screenMaxX / this.blockSize));
        const startRow = Math.max(0, Math.floor(screenMinY / this.blockSize));
        const endRow = Math.min(this.gridHeight - 1, Math.floor(screenMaxY / this.blockSize));
        return { startCol, endCol, startRow, endRow };
    }

    markAreaOpaque(screenMinX, screenMinY, screenMaxX, screenMaxY) {
        const { startCol, endCol, startRow, endRow } = this._getGridIndices(screenMinX, screenMinY, screenMaxX, screenMaxY);
        for (let r = startRow; r <= endRow; r++) {
            for (let c = startCol; c <= endCol; c++) {
                this.grid[r * this.gridWidth + c] = 1; // Mark as opaque
            }
        }
    }

    isAreaOccluded(screenMinX, screenMinY, screenMaxX, screenMaxY) {
        // If area is zero or negative, it can't be occluded in a meaningful way for drawing.
        if (screenMinX >= screenMaxX || screenMinY >= screenMaxY) return false;

        const { startCol, endCol, startRow, endRow } = this._getGridIndices(screenMinX, screenMinY, screenMaxX, screenMaxY);
        for (let r = startRow; r <= endRow; r++) {
            for (let c = startCol; c <= endCol; c++) {
                if (this.grid[r * this.gridWidth + c] === 0) {
                    return false; // Found an empty block, so not fully occluded
                }
            }
        }
        return true; // All blocks covered by the area are opaque
    }
}

export class DisplayListRenderer {
    constructor(ctx, assetsDefinition, optimizationConfig) {
        this.ctx = ctx;
        this.assets = assetsDefinition; // { "PATTERN_NAME": { width, height, data } }
        this.optimizationConfig = optimizationConfig || {}; // { enableOcclusionCulling: true/false }

        this.drawing = new MicroPatternsDrawing(ctx, {
            batchPixelOperations: this.batchPixelOperations.bind(this)
        });
        if (this.drawing.setOptimizationConfig && this.optimizationConfig) {
            this.drawing.setOptimizationConfig(this.optimizationConfig);
        }

        this.occlusionBuffer = new OcclusionBuffer(ctx.canvas.width, ctx.canvas.height, this.optimizationConfig.occlusionBlockSize || 16);
        
        this.totalItems = 0;
        this.renderedItems = 0;
        this.culledOffScreen = 0;
        this.culledByOcclusion = 0;

        this.pixelBatchBuffer = null;
        this.transformCache = new Map(); // Initialize transform cache
    }

    batchPixelOperations(x, y, color) {
        if (!this.optimizationConfig.enablePixelBatching) {
            this.ctx.fillStyle = color;
            this.ctx.fillRect(Math.trunc(x), Math.trunc(y), 1, 1);
            return;
        }

        if (!this.pixelBatchBuffer) {
            this.pixelBatchBuffer = { operations: [], lastColor: null };
        }
        if (this.pixelBatchBuffer.lastColor !== color && this.pixelBatchBuffer.operations.length > 0) {
            this.flushPixelBatch();
        }
        this.pixelBatchBuffer.lastColor = color;
        this.pixelBatchBuffer.operations.push({ x, y });

        if (this.pixelBatchBuffer.operations.length >= 1000) { // Arbitrary flush threshold
            this.flushPixelBatch();
        }
    }

    flushPixelBatch() {
        if (!this.pixelBatchBuffer || this.pixelBatchBuffer.operations.length === 0) {
            return;
        }
        const { operations, lastColor } = this.pixelBatchBuffer;
        this.ctx.fillStyle = lastColor;
        
        if (operations.length > 100) { // Heuristic for using ImageData
             this.drawing._batchPixelsWithImageData(operations, lastColor); // Use existing optimized method
        } else {
            this.ctx.beginPath();
            for (const op of operations) {
                this.ctx.rect(Math.trunc(op.x), Math.trunc(op.y), 1, 1);
            }
            this.ctx.fill();
        }
        this.pixelBatchBuffer.operations = [];
        this.pixelBatchBuffer.lastColor = null;
    }

    _calculateScreenBounds(item) {
        const { logicalParams, transformMatrix, scaleFactor, type } = item;
        let minX, minY, maxX, maxY;
        let isOffScreenResult;

        // Local helper for transforming points for non-asset primitives
        // Apply transform first, then scale for primitives
        const transformPrimitivePoint = (lx, ly) => {
            // First transform the point using the matrix
            const transformedPoint = transformMatrix.transformPoint({ x: lx, y: ly });
            
            // For primitives, we apply scale to the final position
            // This matches the behavior in the interpreter and compiler
            if (type !== 'DRAW' && type !== 'DRAW_ASSET') {
                return {
                    x: transformedPoint.x,
                    y: transformedPoint.y
                };
            } else {
                return transformedPoint;
            }
        };

        if (type === 'DRAW' || type === 'DRAW_ASSET') {
            const assetName = logicalParams.NAME;
            const asset = this.assets[assetName];
            if (!asset || asset.width <= 0 || asset.height <= 0) {
                return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true };
            }
            
            // For DRAW commands, we need to handle scale differently
            // Scale affects the asset size, not the position
            const assetBounds = this.drawing.getAssetScreenBounds(
                logicalParams.X,
                logicalParams.Y,
                asset,
                transformMatrix,
                scaleFactor
            );
            
            minX = assetBounds.minX;
            minY = assetBounds.minY;
            maxX = assetBounds.maxX;
            maxY = assetBounds.maxY;
            
            // Important: Only consider it off-screen if it's completely outside the canvas
            isOffScreenResult = (maxX <= 0 || minX >= this.ctx.canvas.width ||
                               maxY <= 0 || minY >= this.ctx.canvas.height);
        } else {
            // Calculate unclipped bounds for other primitive types
            if (type === 'FILL_RECT' || type === 'RECT') {
                const w = logicalParams.WIDTH * scaleFactor;
                const h = logicalParams.HEIGHT * scaleFactor;
                if (w <= 0 || h <= 0) return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true };

                // Transform the corners
                const p1 = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                const p2 = transformPrimitivePoint(logicalParams.X + logicalParams.WIDTH, logicalParams.Y);
                const p3 = transformPrimitivePoint(logicalParams.X + logicalParams.WIDTH, logicalParams.Y + logicalParams.HEIGHT);
                const p4 = transformPrimitivePoint(logicalParams.X, logicalParams.Y + logicalParams.HEIGHT);

                // Apply scale to the size
                const scaledWidth = logicalParams.WIDTH * scaleFactor;
                const scaledHeight = logicalParams.HEIGHT * scaleFactor;
                
                // Calculate the bounding box
                minX = Math.min(p1.x, p2.x, p3.x, p4.x);
                minY = Math.min(p1.y, p2.y, p3.y, p4.y);
                maxX = Math.max(p1.x, p2.x, p3.x, p4.x);
                maxY = Math.max(p1.y, p2.y, p3.y, p4.y);
                
                // Adjust for scale
                if (scaleFactor > 1) {
                    const centerX = (minX + maxX) / 2;
                    const centerY = (minY + maxY) / 2;
                    const width = maxX - minX;
                    const height = maxY - minY;
                    
                    minX = centerX - (width * scaleFactor) / 2;
                    minY = centerY - (height * scaleFactor) / 2;
                    maxX = centerX + (width * scaleFactor) / 2;
                    maxY = centerY + (height * scaleFactor) / 2;
                }
            } else if (type === 'FILL_CIRCLE' || type === 'CIRCLE') {
                const center = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                const r = logicalParams.RADIUS * scaleFactor;
                if (r <= 0) return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true };

                // Apply scale to the radius
                const screenRadius = r;

                minX = center.x - screenRadius;
                minY = center.y - screenRadius;
                maxX = center.x + screenRadius;
                maxY = center.y + screenRadius;
            } else if (type === 'LINE') {
                const p1 = transformPrimitivePoint(logicalParams.X1, logicalParams.Y1);
                const p2 = transformPrimitivePoint(logicalParams.X2, logicalParams.Y2);
                
                // Calculate the bounding box
                minX = Math.min(p1.x, p2.x);
                minY = Math.min(p1.y, p2.y);
                maxX = Math.max(p1.x, p2.x);
                maxY = Math.max(p1.y, p2.y);
                
                // Adjust for scale
                if (scaleFactor > 1) {
                    const centerX = (minX + maxX) / 2;
                    const centerY = (minY + maxY) / 2;
                    const width = maxX - minX;
                    const height = maxY - minY;
                    
                    minX = centerX - (width * scaleFactor) / 2;
                    minY = centerY - (height * scaleFactor) / 2;
                    maxX = centerX + (width * scaleFactor) / 2;
                    maxY = centerY + (height * scaleFactor) / 2;
                }
            } else if (type === 'PIXEL' || type === 'FILL_PIXEL') {
                const p = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                
                // For a pixel, the size is determined by the scale factor
                const pixelSize = Math.max(1, scaleFactor);
                
                minX = p.x;
                minY = p.y;
                maxX = p.x + pixelSize;
                maxY = p.y + pixelSize;
            } else {
                return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true }; // Unknown type
            }

            // For non-DRAW_ASSET types, determine isOffScreen based on their calculated (unclipped) bounds
            // Important: Only consider it off-screen if it's completely outside the canvas
            isOffScreenResult = (maxX <= 0 || minX >= this.ctx.canvas.width ||
                               maxY <= 0 || minY >= this.ctx.canvas.height);
            
            // Then, clip these bounds to the canvas for rendering purposes
            minX = Math.max(0, minX);
            minY = Math.max(0, minY);
            maxX = Math.min(this.ctx.canvas.width, maxX);
            maxY = Math.min(this.ctx.canvas.height, maxY);
        }

        const finalMinX = Math.trunc(minX);
        const finalMinY = Math.trunc(minY);
        const finalMaxX = Math.ceil(maxX);
        const finalMaxY = Math.ceil(maxY);
        
        if (finalMinX >= finalMaxX || finalMinY >= finalMaxY) {
            isOffScreenResult = true;
        }

        return {
            minX: finalMinX,
            minY: finalMinY,
            maxX: finalMaxX,
            maxY: finalMaxY,
            isOffScreen: isOffScreenResult
        };
    }

    _renderItem(item) {
        const itemState = {
            color: item.color,
            fillAsset: item.fillAsset,
            scaleFactor: item.scaleFactor, // Use scaleFactor consistently
            scale: item.scaleFactor, // Keep 'scale' for compatibility if drawing.js uses it
            matrix: item.transformMatrix,
            inverseMatrix: item.transformMatrix.inverse(),
            transformCache: this.transformCache // Pass renderer's cache
        };

        switch (item.type) {
            case 'FILL_RECT':
                this.drawing.fillRect(item.logicalParams.X, item.logicalParams.Y, item.logicalParams.WIDTH, item.logicalParams.HEIGHT, itemState);
                break;
            case 'RECT':
                this.drawing.drawRect(item.logicalParams.X, item.logicalParams.Y, item.logicalParams.WIDTH, item.logicalParams.HEIGHT, itemState);
                break;
            case 'FILL_CIRCLE':
                this.drawing.fillCircle(item.logicalParams.X, item.logicalParams.Y, item.logicalParams.RADIUS, itemState);
                break;
            case 'CIRCLE':
                this.drawing.drawCircle(item.logicalParams.X, item.logicalParams.Y, item.logicalParams.RADIUS, itemState);
                break;
            case 'LINE':
                this.drawing.drawLine(item.logicalParams.X1, item.logicalParams.Y1, item.logicalParams.X2, item.logicalParams.Y2, itemState);
                break;
            case 'PIXEL':
                this.drawing.drawPixel(item.logicalParams.X, item.logicalParams.Y, itemState);
                break;
            case 'FILL_PIXEL':
                this.drawing.drawFilledPixel(item.logicalParams.X, item.logicalParams.Y, itemState);
                break;
            case 'DRAW':
            case 'DRAW_ASSET':
                const assetToDraw = this.assets[item.logicalParams.NAME];
                if (assetToDraw) {
                    this.drawing.drawAsset(item.logicalParams.X, item.logicalParams.Y, assetToDraw, itemState);
                } else {
                    console.warn(`DisplayListRenderer (Line ${item.sourceLine}): Asset "${item.logicalParams.NAME}" not found for DRAW.`);
                }
                break;
            default:
                console.warn(`DisplayListRenderer (Line ${item.sourceLine}): Unknown item type "${item.type}"`);
        }
    }

    render(displayList) {
        this.totalItems = displayList.length;
        this.renderedItems = 0;
        this.culledOffScreen = 0;
        this.culledByOcclusion = 0;
        this.occlusionBuffer.reset();
        this.transformCache.clear(); // Reset transform cache for each render pass
    
        // Clear canvas (typically white background)
        this.ctx.fillStyle = 'white';
        this.ctx.fillRect(0, 0, this.ctx.canvas.width, this.ctx.canvas.height);
    
        // Initialize pixel occupation map for locking
        if (!this.pixelOccupationMap) {
            this.pixelOccupationMap = new Uint8Array(this.ctx.canvas.width * this.ctx.canvas.height);
        }
        this.pixelOccupationMap.fill(0); // Reset to all unoccupied
        
        // Give drawing access to the pixel occupation map
        this.drawing.pixelOccupationMap = this.pixelOccupationMap;
        this.drawing.display_width = this.ctx.canvas.width;
        this.drawing.display_height = this.ctx.canvas.height;
        
        // Process items in front-to-back order (reversed from original script order)
        // This is crucial for proper occlusion culling with painter's algorithm
        for (let i = displayList.length - 1; i >= 0; i--) {
            const item = displayList[i];
    
            const screenBounds = this._calculateScreenBounds(item);
    
            if (screenBounds.isOffScreen) {
                this.culledOffScreen++;
                continue;
            }
            
            if (screenBounds.minX >= screenBounds.maxX || screenBounds.minY >= screenBounds.maxY) {
                this.culledOffScreen++;
                continue;
            }
    
            if (this.optimizationConfig.enableOcclusionCulling && item.isOpaque) {
                if (this.occlusionBuffer.isAreaOccluded(screenBounds.minX, screenBounds.minY, screenBounds.maxX, screenBounds.maxY)) {
                    this.culledByOcclusion++;
                    this.occlusionBuffer.culledByOcclusionCount++;
                    continue;
                }
            }
    
            this._renderItem(item);
            this.renderedItems++;
    
            if (this.optimizationConfig.enableOcclusionCulling && item.isOpaque) {
                this.occlusionBuffer.markAreaOpaque(screenBounds.minX, screenBounds.minY, screenBounds.maxX, screenBounds.maxY);
            }
        }
        this.flushPixelBatch(); // Final flush for any remaining pixels
    }

    getStats() {
        return {
            totalItems: this.totalItems,
            renderedItems: this.renderedItems,
            culledOffScreen: this.culledOffScreen,
            culledByOcclusion: this.culledByOcclusion,
            occlusionBufferStats: {
                blockSize: this.occlusionBuffer.blockSize,
                gridWidth: this.occlusionBuffer.gridWidth,
                gridHeight: this.occlusionBuffer.gridHeight,
                culledByOcclusionInternal: this.occlusionBuffer.culledByOcclusionCount
            }
        };
    }
}