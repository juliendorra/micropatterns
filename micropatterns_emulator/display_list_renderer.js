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
        let unclippedVisualMinX, unclippedVisualMinY, unclippedVisualMaxX, unclippedVisualMaxY;
        let s_center_for_circle; // To store s_center for FILL_CIRCLE marking bounds adjustment
        let effective_radius_for_circle; // To store effective_screen_radius for FILL_CIRCLE
    
        // Local helper for transforming points for non-asset primitives
        const transformPrimitivePoint = (lx, ly) => {
            const scaledX = lx * scaleFactor;
            const scaledY = ly * scaleFactor;
            return transformMatrix.transformPoint({ x: scaledX, y: scaledY });
        };
    
        if (type === 'DRAW' || type === 'DRAW_ASSET') {
            const assetName = logicalParams.NAME;
            const asset = this.assets[assetName];
            if (!asset || asset.width <= 0 || asset.height <= 0) {
                return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true, markingBounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } };
            }
            const assetBounds = this.drawing.getAssetScreenBounds(logicalParams.X, logicalParams.Y, asset, transformMatrix, scaleFactor);
            unclippedVisualMinX = assetBounds.minX;
            unclippedVisualMinY = assetBounds.minY;
            unclippedVisualMaxX = assetBounds.maxX;
            unclippedVisualMaxY = assetBounds.maxY;
        } else {
            if (type === 'FILL_RECT' || type === 'RECT') {
                if (logicalParams.WIDTH <= 0 || logicalParams.HEIGHT <= 0) {
                     return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true, markingBounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } };
                }
                const p1 = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                const p2 = transformPrimitivePoint(logicalParams.X + logicalParams.WIDTH, logicalParams.Y);
                const p3 = transformPrimitivePoint(logicalParams.X + logicalParams.WIDTH, logicalParams.Y + logicalParams.HEIGHT);
                const p4 = transformPrimitivePoint(logicalParams.X, logicalParams.Y + logicalParams.HEIGHT);
                unclippedVisualMinX = Math.min(p1.x, p2.x, p3.x, p4.x);
                unclippedVisualMinY = Math.min(p1.y, p2.y, p3.y, p4.y);
                unclippedVisualMaxX = Math.max(p1.x, p2.x, p3.x, p4.x);
                unclippedVisualMaxY = Math.max(p1.y, p2.y, p3.y, p4.y);
            } else if (type === 'LINE') {
                const p1 = transformPrimitivePoint(logicalParams.X1, logicalParams.Y1);
                const p2 = transformPrimitivePoint(logicalParams.X2, logicalParams.Y2);
                unclippedVisualMinX = Math.min(p1.x, p2.x);
                unclippedVisualMinY = Math.min(p1.y, p2.y);
                unclippedVisualMaxX = Math.max(p1.x, p2.x);
                unclippedVisualMaxY = Math.max(p1.y, p2.y);
            } else if (type === 'PIXEL' || type === 'FILL_PIXEL') {
                const p = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                const pixelSize = Math.max(1, scaleFactor);
                unclippedVisualMinX = p.x;
                unclippedVisualMinY = p.y;
                unclippedVisualMaxX = p.x + pixelSize;
                unclippedVisualMaxY = p.y + pixelSize;
            } else if (type === 'CIRCLE' || type === 'FILL_CIRCLE') {
                if (logicalParams.RADIUS <= 0) {
                    return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true, markingBounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } };
                }
                s_center_for_circle = transformPrimitivePoint(logicalParams.X, logicalParams.Y);
                const s_edge_on_x_axis = transformPrimitivePoint(logicalParams.X + logicalParams.RADIUS, logicalParams.Y);
                const s_edge_on_y_axis = transformPrimitivePoint(logicalParams.X, logicalParams.Y + logicalParams.RADIUS);
                const radius_projection_x = Math.hypot(s_edge_on_x_axis.x - s_center_for_circle.x, s_edge_on_x_axis.y - s_center_for_circle.y);
                const radius_projection_y = Math.hypot(s_edge_on_y_axis.x - s_center_for_circle.x, s_edge_on_y_axis.y - s_center_for_circle.y);
                effective_radius_for_circle = Math.max(radius_projection_x, radius_projection_y, 1.0);
                unclippedVisualMinX = s_center_for_circle.x - effective_radius_for_circle;
                unclippedVisualMaxX = s_center_for_circle.x + effective_radius_for_circle;
                unclippedVisualMinY = s_center_for_circle.y - effective_radius_for_circle;
                unclippedVisualMaxY = s_center_for_circle.y + effective_radius_for_circle;
            } else {
                return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true, markingBounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } }; // Unknown type
            }
        }
    
        let isOffScreenResult = (unclippedVisualMaxX <= 0 || unclippedVisualMinX >= this.ctx.canvas.width ||
                                 unclippedVisualMaxY <= 0 || unclippedVisualMinY >= this.ctx.canvas.height);
    
        // Calculate final visual bounds (clipped and integerized)
        const finalVisualMinX = Math.trunc(Math.max(0, unclippedVisualMinX));
        const finalVisualMinY = Math.trunc(Math.max(0, unclippedVisualMinY));
        const finalVisualMaxX = Math.ceil(Math.min(this.ctx.canvas.width, unclippedVisualMaxX));
        const finalVisualMaxY = Math.ceil(Math.min(this.ctx.canvas.height, unclippedVisualMaxY));
    
        if (finalVisualMinX >= finalVisualMaxX || finalVisualMinY >= finalVisualMaxY) {
            isOffScreenResult = true;
        }
        
        // Initialize unclipped marking bounds from unclipped visual bounds
        let unclippedMarkingMinX = unclippedVisualMinX;
        let unclippedMarkingMinY = unclippedVisualMinY;
        let unclippedMarkingMaxX = unclippedVisualMaxX;
        let unclippedMarkingMaxY = unclippedVisualMaxY;
    
        if (type === 'FILL_CIRCLE' && item.isOpaque && s_center_for_circle && effective_radius_for_circle > 0) {
            const markingRadiusScaleFactor = Math.sqrt(Math.PI / 4); // approx 0.886
            const markingRadius = effective_radius_for_circle * markingRadiusScaleFactor;
            unclippedMarkingMinX = s_center_for_circle.x - markingRadius;
            unclippedMarkingMaxX = s_center_for_circle.x + markingRadius;
            unclippedMarkingMinY = s_center_for_circle.y - markingRadius;
            unclippedMarkingMaxY = s_center_for_circle.y + markingRadius;
        } else if (type === 'FILL_RECT' && item.isOpaque) {
            const lw = logicalParams.WIDTH;
            const lh = logicalParams.HEIGHT;
    
            if (lw > 0 && lh > 0) {
                const currentItemScaleFactor = item.scaleFactor; // item.scaleFactor is the one applied before matrix
                const matrix = item.transformMatrix;
                
                // Determinant of the 2x2 part of the matrix (m11, m12, m21, m22)
                // This gives the area scaling factor of the matrix itself.
                const matrixDeterminant = Math.abs(matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21);
                
                // Actual screen area of the parallelogram
                const actualScreenArea = lw * lh * currentItemScaleFactor * currentItemScaleFactor * matrixDeterminant;
    
                const visualAABBWidth = unclippedVisualMaxX - unclippedVisualMinX;
                const visualAABBHeight = unclippedVisualMaxY - unclippedVisualMinY;
                const visualAABBArea = visualAABBWidth * visualAABBHeight;
    
                if (actualScreenArea > 0 && visualAABBArea > 0) {
                    const fillFactor = actualScreenArea / visualAABBArea;
                    // Ensure fillFactor is not > 1 (due to float precision) and is positive
                    const effectiveFillFactor = Math.max(0, Math.min(1.0, fillFactor));
    
                    // Only adjust if fill factor is low enough to matter (e.g., less than 0.85)
                    if (effectiveFillFactor < 0.85 && effectiveFillFactor > 0) {
                        const scaleDownFactor = Math.sqrt(effectiveFillFactor);
                        
                        const markingWidth = visualAABBWidth * scaleDownFactor;
                        const markingHeight = visualAABBHeight * scaleDownFactor;
    
                        const centerX = (unclippedVisualMinX + unclippedVisualMaxX) / 2;
                        const centerY = (unclippedVisualMinY + unclippedVisualMaxY) / 2;
    
                        unclippedMarkingMinX = centerX - markingWidth / 2;
                        unclippedMarkingMaxX = centerX + markingWidth / 2;
                        unclippedMarkingMinY = centerY - markingHeight / 2;
                        unclippedMarkingMaxY = centerY + markingHeight / 2;
                    }
                    // Else, if fillFactor is high, marking bounds remain same as visual bounds
                }
                // Else (area is zero), marking bounds remain same as visual bounds
            }
            // Else (lw or lh is zero), marking bounds remain same as visual bounds
        }
    
    
        // Calculate final marking bounds (clipped and integerized)
        const finalMarkingMinX = Math.trunc(Math.max(0, unclippedMarkingMinX));
        const finalMarkingMinY = Math.trunc(Math.max(0, unclippedMarkingMinY));
        const finalMarkingMaxX = Math.ceil(Math.min(this.ctx.canvas.width, unclippedMarkingMaxX));
        const finalMarkingMaxY = Math.ceil(Math.min(this.ctx.canvas.height, unclippedMarkingMaxY));
    
        return {
            minX: finalVisualMinX,
            minY: finalVisualMinY,
            maxX: finalVisualMaxX,
            maxY: finalVisualMaxY,
            isOffScreen: isOffScreenResult,
            markingBounds: {
                minX: finalMarkingMinX,
                minY: finalMarkingMinY,
                maxX: finalMarkingMaxX,
                maxY: finalMarkingMaxY,
            }
        };
    }
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