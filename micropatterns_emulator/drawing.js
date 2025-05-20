export class MicroPatternsDrawing {
    constructor(ctx, optimizationAPI = null) {
        this.ctx = ctx;
        this.display_width = ctx.canvas.width;
        this.display_height = ctx.canvas.height;
        this.optimizationAPI = optimizationAPI;
        this.optimizationConfig = null;
        
        // Pixel occupation tracking for locking pixels once drawn
        this.pixelOccupationMap = null; // Will be set by renderer
        this.overdrawSkippedPixels = 0; // Track stats
        
        // sinTable and cosTable are no longer needed with DOMMatrix
    }
    
    // Reset pixel occupation map and stats
    resetPixelOccupationMap() {
        if (this.pixelOccupationMap) {
            this.pixelOccupationMap.fill(0);
        }
        this.resetOverdrawStats();
    }
    
    // Reset overdraw statistics
    resetOverdrawStats() {
        this.overdrawSkippedPixels = 0;
    }
    
    // Get current overdraw statistics
    getOverdrawStats() {
        return {
            skippedPixels: this.overdrawSkippedPixels
        };
    }
    
    // Check if a pixel is already occupied
    isPixelOccupied(x, y) {
        if (!this.pixelOccupationMap) return false;
        
        const sx = Math.trunc(x);
        const sy = Math.trunc(y);
        
        // Skip if outside canvas bounds
        if (sx < 0 || sx >= this.display_width || sy < 0 || sy >= this.display_height) {
            return false;
        }
        
        const index = sy * this.display_width + sx;
        return this.pixelOccupationMap[index] !== 0;
    }
    
    // Mark a pixel as occupied with a specific color (1 for black, 2 for white)
    markPixelOccupied(x, y, color) {
        if (!this.pixelOccupationMap) return;
        
        const sx = Math.trunc(x);
        const sy = Math.trunc(y);
        
        // Skip if outside canvas bounds
        if (sx < 0 || sx >= this.display_width || sy < 0 || sy >= this.display_height) {
            return;
        }
        
        const index = sy * this.display_width + sx;
        const value = color === 'black' ? 1 : 2; // Distinguish colors for potential future use
        this.pixelOccupationMap[index] = value;
    }

    setOptimizationConfig(config) {
        this.optimizationConfig = config;
    }

    // Internal Bresenham's line algorithm operating on SCREEN coordinates.
    // Draws individual 1x1 screen pixels for the line.
    // Accepts itemState for color, or falls back to a simple state object if itemState is not fully featured.
    _rawLine(sx1, sy1, sx2, sy2, itemState) {
        let x1 = Math.trunc(sx1);
        let y1 = Math.trunc(sy1);
        const x2 = Math.trunc(sx2);
        const y2 = Math.trunc(sy2);

        const dx = Math.abs(x2 - x1);
        const dy = -Math.abs(y2 - y1);
        const stepX = x1 < x2 ? 1 : -1;
        const stepY = y1 < y2 ? 1 : -1;
        let err = dx + dy;

        this.ctx.fillStyle = itemState.color;

        while (true) {
            if (x1 >= 0 && x1 < this.display_width && y1 >= 0 && y1 < this.display_height) {
                this.ctx.fillRect(x1, y1, 1, 1); // Draw 1x1 screen pixel
            }

            if (x1 === x2 && y1 === y2) break;

            const e2 = 2 * err;
            let moved = false;
            if (e2 >= dy) {
                if (x1 === x2) break;
                err += dy;
                x1 += stepX;
                moved = true;
            }
            if (e2 <= dx) {
                if (y1 === y2) break;
                err += dx;
                y1 += stepY;
                moved = true;
            }
            if (!moved) break; // Safety break
        }
    }

    // --- Transformation ---

    // Apply transformations: 1. itemState.scaleFactor, then 2. itemState.transformMatrix
    // Returns transformed { x, y } screen coordinates (float).
    transformPoint(logicalX, logicalY, itemState) {
        const scaleToUse = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);
        const matrixToUse = itemState.transformMatrix || itemState.matrix;

        const scaledX = logicalX * scaleToUse;
        const scaledY = logicalY * scaleToUse;

        // Optimization: Cache common coordinates in a state-based cache
        // The transformCache should ideally be managed by the caller (runner) if it's context-dependent.
        // For simplicity, if itemState has a transformCache, use it.
        if (itemState.transformCache) {
            // Ensure matrixToUse is defined before using its properties in the key
            if (!matrixToUse) {
                console.error("transformPoint: matrixToUse is undefined for cache key generation", itemState);
                // Proceed without caching if matrix is missing, or handle error appropriately
            } else {
                const key = `${Math.trunc(scaledX)},${Math.trunc(scaledY)},${matrixToUse.a},${matrixToUse.b},${matrixToUse.c},${matrixToUse.d},${matrixToUse.e},${matrixToUse.f}`;
                if (itemState.transformCache.has(key)) {
                    return itemState.transformCache.get(key);
                }
            }
        }

        if (!matrixToUse) {
            console.error("transformPoint: matrixToUse is undefined", itemState);
            return { x: scaledX, y: scaledY }; // Fallback if matrix is missing
        }
        const screenPoint = matrixToUse.transformPoint({ x: scaledX, y: scaledY });
        const result = { x: screenPoint.x, y: screenPoint.y };
        
        if (itemState.transformCache && Number.isInteger(scaledX) && Number.isInteger(scaledY) && matrixToUse) {
            const key = `${Math.trunc(scaledX)},${Math.trunc(scaledY)},${matrixToUse.a},${matrixToUse.b},${matrixToUse.c},${matrixToUse.d},${matrixToUse.e},${matrixToUse.f}`;
            itemState.transformCache.set(key, result);
            if (itemState.transformCache.size > 1000) { // Basic cache eviction
                const keys = Array.from(itemState.transformCache.keys());
                for (let i = 0; i < 200; i++) itemState.transformCache.delete(keys[i]);
            }
        }
        return result;
    }

    // Inverse transform: screen coordinates to base logical coordinates (pattern space)
    // 1. Apply itemState.inverseMatrix, then 2. Undo itemState.scaleFactor.
    // Returns { x, y } base logical coordinates (float).
    screenToLogicalBase(screenX, screenY, itemState) {
        const matrixToUse = itemState.transformMatrix || itemState.matrix;
        const invMatrix = itemState.inverseMatrix || (matrixToUse ? matrixToUse.inverse() : new DOMMatrix());
        const scaleToUse = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);

        if (!invMatrix) {
            console.error("screenToLogicalBase: invMatrix is undefined", itemState);
            // Fallback: return screen coordinates if inverse transform cannot be applied
            return { x: screenX / (scaleToUse || 1), y: screenY / (scaleToUse || 1) };
        }
        const scaledLogicalPoint = invMatrix.transformPoint({ x: screenX, y: screenY });

        let baseLogicalX = scaledLogicalPoint.x;
        let baseLogicalY = scaledLogicalPoint.y;

        if (scaleToUse !== 0) {
            baseLogicalX /= scaleToUse;
            baseLogicalY /= scaleToUse;
        }
        return { x: baseLogicalX, y: baseLogicalY };
    }


    // Helper function to get the correct pixel color for fills.
    // screenPixelCenterX, screenPixelCenterY are canvas coordinates of the pixel center.
    // Returns the effective color ('black', 'white') or 'white' if the pattern pixel is off.
    _getFillAssetPixelColor(screenPixelCenterX, screenPixelCenterY, itemState) {
        const fillAsset = itemState.fillAsset;
        const itemColor = itemState.color; // Use itemColor from itemState

        if (!fillAsset) {
            return itemColor; // Solid fill
        }
        
        // Fast path: check for cached pattern tile lookup table
        // The cachedTile should be associated with the asset and the specific itemColor.
        // This logic might need adjustment if cachedTile is on fillAsset directly and shared.
        // For now, assume _initializePatternCache handles this.
        if (fillAsset.cachedTile && fillAsset.cachedTile.color === itemColor) {
            const baseCoords = this.screenToLogicalBase(screenPixelCenterX, screenPixelCenterY, itemState);
            let logicalX = baseCoords.x;
            let logicalY = baseCoords.y;
            
            let assetX = Math.floor(logicalX) % fillAsset.width;
            let assetY = Math.floor(logicalY) % fillAsset.height;
            
            if (assetX < 0) assetX += fillAsset.width;
            if (assetY < 0) assetY += fillAsset.height;
            
            const index = assetY * fillAsset.width + assetX;
            
            if (index < 0 || index >= fillAsset.cachedTile.lookup.length) {
                return itemColor === 'white' ? 'black' : 'white';
            }
            
            const patternBit = fillAsset.data[index];
            
            if (itemColor === 'white') { // Inverted mode for FILL
                return patternBit === 1 ? 'white' : 'black';
            } else { // Normal mode (itemColor is 'black') for FILL
                return patternBit === 1 ? 'black' : 'white';
            }
        }
        
        // Original path
        const baseCoords = this.screenToLogicalBase(screenPixelCenterX, screenPixelCenterY, itemState);
        let logicalX = baseCoords.x;
        let logicalY = baseCoords.y;

        let assetX = Math.floor(logicalX) % fillAsset.width;
        let assetY = Math.floor(logicalY) % fillAsset.height;
        
        if (assetX < 0) assetX += fillAsset.width;
        if (assetY < 0) assetY += fillAsset.height;

        const index = assetY * fillAsset.width + assetX;

        if (index < 0 || index >= fillAsset.data.length) {
            return itemColor === 'white' ? 'black' : 'white';
        }

        const patternBit = fillAsset.data[index];

        if (itemColor === 'white') { // Inverted mode for FILL
            return patternBit === 1 ? 'white' : 'black';
        } else { // Normal mode (itemColor is 'black') for FILL
            return patternBit === 1 ? 'black' : 'white';
        }
    }
    
    // Initialize cached pattern lookups
    _initializePatternCache(fillAsset, itemColor) { // Takes itemColor
        if (!fillAsset) return;
        
        if (!fillAsset.cachedTile) {
            fillAsset.cachedTile = {
                lookup: new Uint8Array(fillAsset.width * fillAsset.height),
                width: fillAsset.width,
                height: fillAsset.height,
                color: itemColor // Store with itemColor
            };
            for (let y = 0; y < fillAsset.height; y++) {
                for (let x = 0; x < fillAsset.width; x++) {
                    const index = y * fillAsset.width + x;
                    const patternBit = fillAsset.data[index];
                    if (itemColor === 'white') {
                        fillAsset.cachedTile.lookup[index] = patternBit === 1 ? 1 : 0;
                    } else {
                        fillAsset.cachedTile.lookup[index] = patternBit === 1 ? 0 : 1;
                    }
                }
            }
        } else if (fillAsset.cachedTile.color !== itemColor) {
            fillAsset.cachedTile.color = itemColor;
            for (let i = 0; i < fillAsset.cachedTile.lookup.length; i++) {
                fillAsset.cachedTile.lookup[i] = 1 - fillAsset.cachedTile.lookup[i];
            }
        }
    }

    // Helper function to set a single "logical pixel" block on the canvas, considering scale.
    // screenX, screenY are the top-left coordinates of the block on the canvas.
    // This is used by DRAW_PIXEL and DRAW_FILLED_PIXEL.
    setPixel(screenX, screenY, color, itemState) { // Takes itemState for scaleFactor
        const sx = Math.trunc(screenX);
        const sy = Math.trunc(screenY);
        const scaleToUse = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);
        const scale = Math.round(scaleToUse);
        
        // Check if this pixel is already occupied - pixel locking
        if (this.pixelOccupationMap) {
            const isOccupied = this.isPixelOccupied(sx, sy);
            if (isOccupied) {
                this.overdrawSkippedPixels++;
                return; // Skip drawing if pixel is already occupied
            }
            
            // Mark this pixel as occupied before drawing
            this.markPixelOccupied(sx, sy, color);
        }
        
        this.ctx.fillStyle = color;
        this.ctx.fillRect(sx, sy, scale, scale);
    }
    
    // Efficient batch pixel drawing - use for multiple pixels of the same color
    // Takes an array of { x, y } screen coordinates and draws them all at once
    batchPixels(pixelArray, color, scale = 1) {
        if (pixelArray.length === 0) return;
        
        this.ctx.fillStyle = color;
        this.ctx.beginPath();
        
        const scaledSize = Math.round(scale);
        
        // Special case optimization: if we have many pixels, use ImageData for better performance
        if (pixelArray.length > 100 && scale === 1) {
            this._batchPixelsWithImageData(pixelArray, color);
            return;
        }
        
        for (const pixel of pixelArray) {
            const x = Math.trunc(pixel.x);
            const y = Math.trunc(pixel.y);
            this.ctx.rect(x, y, scaledSize, scaledSize);
        }
        
        this.ctx.fill();
    }
    
    // Optimized pixel rendering using ImageData for large batches
    _batchPixelsWithImageData(pixelArray, color) {
        // Get color components
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
        
        // Find bounds of the pixel array
        let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
        for (const pixel of pixelArray) {
            const x = Math.trunc(pixel.x);
            const y = Math.trunc(pixel.y);
            minX = Math.min(minX, x);
            minY = Math.min(minY, y);
            maxX = Math.max(maxX, x);
            maxY = Math.max(maxY, y);
        }
        
        // Clip to canvas boundaries
        minX = Math.max(0, minX);
        minY = Math.max(0, minY);
        maxX = Math.min(this.display_width - 1, maxX);
        maxY = Math.min(this.display_height - 1, maxY);
        
        // Create ImageData covering the area
        const width = maxX - minX + 1;
        const height = maxY - minY + 1;
        
        // Skip if dimensions are invalid
        if (width <= 0 || height <= 0) return;
        
        // Get current ImageData if we need to preserve background
        const imageData = this.ctx.getImageData(minX, minY, width, height);
        const data = imageData.data;
        
        // Set pixels in the ImageData
        for (const pixel of pixelArray) {
            const x = Math.trunc(pixel.x) - minX;
            const y = Math.trunc(pixel.y) - minY;
            
            // Skip if pixel is outside bounds
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            // Calculate index in the data array (4 bytes per pixel: r,g,b,a)
            const index = (y * width + x) * 4;
            data[index] = r;
            data[index + 1] = g;
            data[index + 2] = b;
            data[index + 3] = a;
        }
        
        // Put the ImageData back to the canvas
        this.ctx.putImageData(imageData, minX, minY);
    }

    // New method to calculate screen bounds for an asset based on "Interpretation B"
    // assetLogicalX, assetLogicalY are NOT pre-scaled by currentScaleFactor.
    // asset dimensions ARE scaled by currentScaleFactor.
    // The resulting shape is then transformed by currentTransformMatrix.
    getAssetScreenBounds(assetLogicalX, assetLogicalY, assetDefinition, currentTransformMatrix, currentScaleFactor) {
        if (!assetDefinition || assetDefinition.width <= 0 || assetDefinition.height <= 0) {
            return { minX: 0, minY: 0, maxX: 0, maxY: 0, isOffScreen: true };
        }

        const scaledAssetWidth = assetDefinition.width * currentScaleFactor;
        const scaledAssetHeight = assetDefinition.height * currentScaleFactor;

        // Define corners in the logical space, using assetLogicalX,Y as the origin for the SCALED asset.
        // These points (assetLogicalX, assetLogicalY) are NOT themselves scaled by currentScaleFactor yet.
        const p_local_tl = { x: assetLogicalX, y: assetLogicalY };
        const p_local_tr = { x: assetLogicalX + scaledAssetWidth, y: assetLogicalY };
        const p_local_br = { x: assetLogicalX + scaledAssetWidth, y: assetLogicalY + scaledAssetHeight };
        const p_local_bl = { x: assetLogicalX, y: assetLogicalY + scaledAssetHeight };

        // Transform these points using the provided matrix.
        const s_tl = currentTransformMatrix.transformPoint(p_local_tl);
        const s_tr = currentTransformMatrix.transformPoint(p_local_tr);
        const s_br = currentTransformMatrix.transformPoint(p_local_br);
        const s_bl = currentTransformMatrix.transformPoint(p_local_bl);

        let minX = Math.min(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let minY = Math.min(s_tl.y, s_tr.y, s_br.y, s_bl.y);
        let maxX = Math.max(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let maxY = Math.max(s_tl.y, s_tr.y, s_br.y, s_bl.y);

        const isOffScreen = maxX < 0 || minX > this.display_width || maxY < 0 || minY > this.display_height;

        // Clip to canvas bounds for the returned values
        minX = Math.max(0, minX);
        minY = Math.max(0, minY);
        maxX = Math.min(this.display_width, maxX);
        maxY = Math.min(this.display_height, maxY);
        
        return { minX, minY, maxX, maxY, isOffScreen };
    }


    // --- Drawing Primitives ---

    drawLine(x1, y1, x2, y2, itemState) {
        const p1 = this.transformPoint(x1, y1, itemState);
        const p2 = this.transformPoint(x2, y2, itemState);
        this._rawLine(p1.x, p1.y, p2.x, p2.y, itemState);
    }

    drawRect(x, y, width, height, itemState) {
        const p1 = this.transformPoint(x, y, itemState);
        const p2 = this.transformPoint(x + width -1, y, itemState);
        const p3 = this.transformPoint(x + width -1, y + height -1, itemState);
        const p4 = this.transformPoint(x, y + height -1, itemState);

        this._rawLine(p1.x, p1.y, p2.x, p2.y, itemState);
        this._rawLine(p2.x, p2.y, p3.x, p3.y, itemState);
        this._rawLine(p3.x, p3.y, p4.x, p4.y, itemState);
        this._rawLine(p4.x, p4.y, p1.x, p1.y, itemState);
    }
    
    fillRect(lx, ly, lw, lh, itemState) {
        if (lw <= 0 || lh <= 0) return;

        const currentTransformMatrix = itemState.transformMatrix || itemState.matrix;
        const currentScaleFactor = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);

        if (!currentTransformMatrix) {
            console.error("fillRect: currentTransformMatrix is undefined", itemState);
            return; // Cannot proceed without a matrix
        }
        
        // Fast path optimization for non-rotated rectangles using solid fill
        if (!itemState.fillAsset && currentTransformMatrix.b === 0 && currentTransformMatrix.c === 0) {
            // For axis-aligned rectangles with solid fill, use the built-in fillRect
            const scaledX = lx * currentScaleFactor;
            const scaledY = ly * currentScaleFactor;
            const scaledW = lw * currentScaleFactor;
            const scaledH = lh * currentScaleFactor;
            
            const screenX = scaledX * currentTransformMatrix.a + currentTransformMatrix.e;
            const screenY = scaledY * currentTransformMatrix.d + currentTransformMatrix.f;
            const screenW = scaledW * currentTransformMatrix.a;
            const screenH = scaledH * currentTransformMatrix.d;
            
            const sx = Math.trunc(screenX);
            const sy = Math.trunc(screenY);
            const sw = Math.ceil(screenW);
            const sh = Math.ceil(screenH);
            
            // If we have pixel locking enabled, need to check each pixel in the rect
            if (this.pixelOccupationMap) {
                // For large rectangles, check individual pixels
                for (let y = sy; y < sy + sh; y++) {
                    for (let x = sx; x < sx + sw; x++) {
                        if (!this.isPixelOccupied(x, y)) {
                            this.ctx.fillStyle = itemState.color;
                            this.ctx.fillRect(x, y, 1, 1);
                            this.markPixelOccupied(x, y, itemState.color);
                        } else {
                            this.overdrawSkippedPixels++;
                        }
                    }
                }
            } else {
                // Without pixel locking, use regular fillRect
                this.ctx.fillStyle = itemState.color;
                this.ctx.fillRect(sx, sy, sw, sh);
            }
            return;
        }
        
        if (itemState.fillAsset) {
            this._initializePatternCache(itemState.fillAsset, itemState.color);
        }

        // 1. Transform logical corners to screen space
        const s_tl = this.transformPoint(lx, ly, itemState);
        const s_tr = this.transformPoint(lx + lw, ly, itemState);
        const s_br = this.transformPoint(lx + lw, ly + lh, itemState);
        const s_bl = this.transformPoint(lx, ly + lh, itemState);

        // 2. Determine screen-space bounding box
        let min_sx = Math.min(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let max_sx = Math.max(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let min_sy = Math.min(s_tl.y, s_tr.y, s_br.y, s_bl.y);
        let max_sy = Math.max(s_tl.y, s_tr.y, s_br.y, s_bl.y);

        // Clip to canvas (using Math.trunc for integer loop bounds)
        min_sx = Math.max(0, Math.trunc(min_sx));
        min_sy = Math.max(0, Math.trunc(min_sy));
        max_sx = Math.min(this.display_width, Math.ceil(max_sx)); // Use ceil for max to include pixels partially covered
        max_sy = Math.min(this.display_height, Math.ceil(max_sy));
        
        // Optimization: Use bulk ImageData for pattern fills in large areas
        const pixelCount = (max_sx - min_sx) * (max_sy - min_sy);
        if (itemState.fillAsset && pixelCount > 1000 && this.optimizationConfig?.enableFastPathSelection) { // Check fast path config
            this._fillRectWithImageData(lx, ly, lw, lh, itemState, min_sx, min_sy, max_sx, max_sy);
            return;
        }

        // Define the rectangle in scaled logical coordinates (used repeatedly)
        const sl_rect_x = lx * currentScaleFactor;
        const sl_rect_y = ly * currentScaleFactor;
        const sl_rect_w = lw * currentScaleFactor;
        const sl_rect_h = lh * currentScaleFactor;
        
        const useBatching = this.optimizationConfig?.enablePixelBatching && this.optimizationAPI?.batchPixelOperations;
        
        const pixelBatch = useBatching ? [] : null;
        let lastColor = null;
        const invMatrix = itemState.inverseMatrix || (currentTransformMatrix ? currentTransformMatrix.inverse() : new DOMMatrix());

        // 3. Iterate over screen pixels in this bounding box
        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                
                const scaled_logical_pixel = invMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });

                if (scaled_logical_pixel.x >= sl_rect_x && scaled_logical_pixel.x < (sl_rect_x + sl_rect_w) &&
                    scaled_logical_pixel.y >= sl_rect_y && scaled_logical_pixel.y < (sl_rect_y + sl_rect_h)) {
                    
                    // Check pixel locking
                    if (this.pixelOccupationMap) {
                        if (this.isPixelOccupied(sx_iter, sy_iter)) {
                            this.overdrawSkippedPixels++;
                            continue; // Skip already occupied pixels
                        }
                    }
                    
                    const fillColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, itemState);
                    
                    // Mark pixel as occupied after determining its color
                    if (this.pixelOccupationMap) {
                        this.markPixelOccupied(sx_iter, sy_iter, fillColor);
                    }
                    
                    if (useBatching) {
                        // If color changed, flush the batch
                        if (lastColor !== null && lastColor !== fillColor) {
                            this.ctx.fillStyle = lastColor;
                            this.ctx.beginPath();
                            for (const pixel of pixelBatch) {
                                this.ctx.rect(pixel.x, pixel.y, 1, 1);
                            }
                            this.ctx.fill();
                            pixelBatch.length = 0; // Clear the batch
                        }
                        
                        // Add to batch
                        pixelBatch.push({ x: sx_iter, y: sy_iter });
                        lastColor = fillColor;
                    } else {
                        // Direct draw
                        this.ctx.fillStyle = fillColor;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1); // Draw 1x1 screen pixel
                    }
                }
            }
        }
        
        // Flush any remaining pixels in the batch
        if (useBatching && pixelBatch.length > 0) {
            this.ctx.fillStyle = lastColor;
            this.ctx.beginPath();
            for (const pixel of pixelBatch) {
                this.ctx.rect(pixel.x, pixel.y, 1, 1);
            }
            this.ctx.fill();
        // End of batchPixelOperations
    }
    }

    // Optimized rectangle filling using ImageData
    _fillRectWithImageData(lx, ly, lw, lh, itemState, min_sx_bound, min_sy_bound, max_sx_bound, max_sy_bound) {
        const currentTransformMatrix = itemState.transformMatrix || itemState.matrix;
        const currentScaleFactor = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);
        const invMatrix = itemState.inverseMatrix || (currentTransformMatrix ? currentTransformMatrix.inverse() : new DOMMatrix());

        // Calculate dimensions for ImageData
        const imgDataWidth = max_sx_bound - min_sx_bound;
        const imgDataHeight = max_sy_bound - min_sy_bound;

        if (imgDataWidth <= 0 || imgDataHeight <= 0) return;

        const imageData = this.ctx.createImageData(imgDataWidth, imgDataHeight);
        const data = imageData.data;

        // Define the rectangle in scaled logical coordinates
        const sl_rect_x = lx * currentScaleFactor;
        const sl_rect_y = ly * currentScaleFactor;
        const sl_rect_w = lw * currentScaleFactor;
        const sl_rect_h = lh * currentScaleFactor;

        for (let y_offset = 0; y_offset < imgDataHeight; ++y_offset) {
            for (let x_offset = 0; x_offset < imgDataWidth; ++x_offset) {
                const sx_iter = min_sx_bound + x_offset;
                const sy_iter = min_sy_bound + y_offset;

                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;

                const scaled_logical_pixel = invMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });

                if (scaled_logical_pixel.x >= sl_rect_x && scaled_logical_pixel.x < (sl_rect_x + sl_rect_w) &&
                    scaled_logical_pixel.y >= sl_rect_y && scaled_logical_pixel.y < (sl_rect_y + sl_rect_h)) {

                    if (this.pixelOccupationMap) {
                        if (this.isPixelOccupied(sx_iter, sy_iter)) {
                            this.overdrawSkippedPixels++;
                            continue;
                        }
                    }

                    const fillColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, itemState);
                    
                    if (this.pixelOccupationMap) {
                        this.markPixelOccupied(sx_iter, sy_iter, fillColor);
                    }

                    let r, g, b, a_val;
                    if (fillColor === 'black') {
                        r = 0; g = 0; b = 0; a_val = 255;
                    } else if (fillColor === 'white') {
                        r = 255; g = 255; b = 255; a_val = 255;
                    } else {
                        // Fallback for other colors if ever supported, or handle error
                        // For now, assume black or white
                        const tempCanvas = document.createElement('canvas');
                        const tempCtx = tempCanvas.getContext('2d');
                        tempCtx.fillStyle = fillColor;
                        r = parseInt(tempCtx.fillStyle.slice(1, 3), 16);
                        g = parseInt(tempCtx.fillStyle.slice(3, 5), 16);
                        b = parseInt(tempCtx.fillStyle.slice(5, 7), 16);
                        a_val = 255;
                    }

                    const dataIndex = (y_offset * imgDataWidth + x_offset) * 4;
                    data[dataIndex] = r;
                    data[dataIndex + 1] = g;
                    data[dataIndex + 2] = b;
                    data[dataIndex + 3] = a_val;
                }
            }
        }
        this.ctx.putImageData(imageData, min_sx_bound, min_sy_bound);
    }

    drawPixel(x, y, itemState) {
        const p = this.transformPoint(x, y, itemState);
        // setPixel now takes itemState for scaleFactor
        this.setPixel(p.x, p.y, itemState.color, itemState);
    }

    drawCircle(cx, cy, radius, itemState) {
        const center = this.transformPoint(cx, cy, itemState);
        const p_edge = this.transformPoint(cx + radius, cy, itemState);
        const scaledRadius = Math.hypot(p_edge.x - center.x, p_edge.y - center.y);
        const screenRadius = Math.max(1, Math.trunc(scaledRadius));

        let x_coord = screenRadius; // Renamed from x to avoid conflict
        let y_coord = 0;            // Renamed from y
        let err = 1 - screenRadius;
        
        this.ctx.fillStyle = itemState.color;

        while (x_coord >= y_coord) {
            const points = [
                { dx: x_coord, dy: y_coord }, { dx: y_coord, dy: x_coord },
                { dx: -y_coord, dy: x_coord }, { dx: -x_coord, dy: y_coord },
                { dx: -x_coord, dy: -y_coord }, { dx: -y_coord, dy: -x_coord },
                { dx: y_coord, dy: -x_coord }, { dx: x_coord, dy: -y_coord }
            ];

            for (const pt of points) {
                const sx = Math.trunc(center.x + pt.dx);
                const sy = Math.trunc(center.y + pt.dy);
                if (sx >= 0 && sx < this.display_width && sy >= 0 && sy < this.display_height) {
                     this.ctx.fillRect(sx, sy, 1, 1);
                }
            }

            y_coord++;
            if (err <= 0) {
                err += 2 * y_coord + 1;
            } else {
                x_coord--;
                err += 2 * (y_coord - x_coord) + 1;
            }
        }
    }

    fillCircle(lcx, lcy, lr, itemState) {
        if (lr <= 0) return;

        const screen_center = this.transformPoint(lcx, lcy, itemState);
        const logical_edge_x = lcx + lr;
        const logical_edge_y = lcy;
        const screen_edge = this.transformPoint(logical_edge_x, logical_edge_y, itemState);
        let approx_screen_radius = Math.hypot(screen_edge.x - screen_center.x, screen_edge.y - screen_center.y);
        const logical_edge_y_alt = this.transformPoint(lcx, lcy + lr, itemState);
        const approx_screen_radius_y = Math.hypot(logical_edge_y_alt.x - screen_center.x, logical_edge_y_alt.y - screen_center.y);
        approx_screen_radius = Math.max(approx_screen_radius, approx_screen_radius_y, 1.0);

        let min_sx = Math.trunc(screen_center.x - approx_screen_radius);
        let max_sx = Math.ceil(screen_center.x + approx_screen_radius);
        let min_sy = Math.trunc(screen_center.y - approx_screen_radius);
        let max_sy = Math.ceil(screen_center.y + approx_screen_radius);

        min_sx = Math.max(0, min_sx);
        min_sy = Math.max(0, min_sy);
        max_sx = Math.min(this.display_width, max_sx);
        max_sy = Math.min(this.display_height, max_sy);

        const currentTransformMatrix = itemState.transformMatrix || itemState.matrix;
        const currentScaleFactor = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);

        if (!currentTransformMatrix) {
            console.error("fillCircle: currentTransformMatrix is undefined", itemState);
            return;
        }

        const scaled_logical_radius_sq = (lr * currentScaleFactor) * (lr * currentScaleFactor);
        const scaled_logical_cx = lcx * currentScaleFactor;
        const scaled_logical_cy = lcy * currentScaleFactor;
        const invMatrix = itemState.inverseMatrix || (currentTransformMatrix ? currentTransformMatrix.inverse() : new DOMMatrix());

        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                // Check pixel locking
                if (this.pixelOccupationMap) {
                    if (this.isPixelOccupied(sx_iter, sy_iter)) {
                        this.overdrawSkippedPixels++;
                        continue; // Skip already occupied pixels
                    }
                }
                
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                const scaled_logical_pixel = invMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });
                
                const dist_sq = (scaled_logical_pixel.x - scaled_logical_cx) * (scaled_logical_pixel.x - scaled_logical_cx) +
                                (scaled_logical_pixel.y - scaled_logical_cy) * (scaled_logical_pixel.y - scaled_logical_cy);

                if (dist_sq <= scaled_logical_radius_sq) {
                    const fillColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, itemState);
                    
                    // Mark pixel as occupied
                    if (this.pixelOccupationMap) {
                        this.markPixelOccupied(sx_iter, sy_iter, fillColor);
                    }
                    
                    if (this.optimizationConfig && this.optimizationConfig.enablePixelBatching && this.optimizationAPI && this.optimizationAPI.batchPixelOperations) {
                        this.optimizationAPI.batchPixelOperations(sx_iter, sy_iter, fillColor);
                    } else {
                        this.ctx.fillStyle = fillColor;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1);
                    }
                }
            }
        }
    }

    drawFilledPixel(x, y, itemState) {
        const p_top_left_screen = this.transformPoint(x, y, itemState);
        const p_bottom_right_screen = this.transformPoint(x + 1, y + 1, itemState);

        const min_sx = Math.trunc(Math.min(p_top_left_screen.x, p_bottom_right_screen.x));
        const max_sx = Math.ceil(Math.max(p_top_left_screen.x, p_bottom_right_screen.x));
        const min_sy = Math.trunc(Math.min(p_top_left_screen.y, p_bottom_right_screen.y));
        const max_sy = Math.ceil(Math.max(p_top_left_screen.y, p_bottom_right_screen.y));

        const currentTransformMatrix = itemState.transformMatrix || itemState.matrix;
        const currentScaleFactor = (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale);
        
        if (!currentTransformMatrix) {
            console.error("drawFilledPixel: currentTransformMatrix is undefined", itemState);
            return;
        }

        const scaled_lx = x * currentScaleFactor;
        const scaled_ly = y * currentScaleFactor;
        const next_scaled_lx = (x + 1) * currentScaleFactor;
        const next_scaled_ly = (y + 1) * currentScaleFactor;
        const invMatrix = itemState.inverseMatrix || (currentTransformMatrix ? currentTransformMatrix.inverse() : new DOMMatrix());

        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            if (sy_iter < 0 || sy_iter >= this.display_height) continue;
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                if (sx_iter < 0 || sx_iter >= this.display_width) continue;

                // Check pixel locking
                if (this.pixelOccupationMap) {
                    if (this.isPixelOccupied(sx_iter, sy_iter)) {
                        this.overdrawSkippedPixels++;
                        continue; // Skip already occupied pixels
                    }
                }

                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                const slp = invMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });

                if (slp.x >= scaled_lx && slp.x < next_scaled_lx &&
                    slp.y >= scaled_ly && slp.y < next_scaled_ly) {
                    
                    const effectiveColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, itemState);
                    
                    // Mark pixel as occupied
                    if (this.pixelOccupationMap) {
                        this.markPixelOccupied(sx_iter, sy_iter, effectiveColor);
                    }
                    
                    this.ctx.fillStyle = effectiveColor;
                    this.ctx.fillRect(sx_iter, sy_iter, 1, 1);
                }
            }
        }
    }

    drawAsset(lx_asset_origin, ly_asset_origin, assetData, itemState) {
        if (!assetData || assetData.width <= 0 || assetData.height <= 0) return;

        // Use the new getAssetScreenBounds method for determining iteration bounds.
        // itemState.matrix and itemState.scale (which is scaleFactor) are used.
        const bounds = this.getAssetScreenBounds(
            lx_asset_origin,
            ly_asset_origin,
            assetData,
            itemState.matrix, // This is currentTransformMatrix
            (itemState.scaleFactor !== undefined ? itemState.scaleFactor : itemState.scale) // This is currentScaleFactor
        );

        if (bounds.isOffScreen) return;

        // Use Math.trunc for integer loop bounds, and Math.ceil for max to include partially covered pixels.
        const min_sx = Math.max(0, Math.trunc(bounds.minX));
        const min_sy = Math.max(0, Math.trunc(bounds.minY));
        const max_sx = Math.min(this.display_width, Math.ceil(bounds.maxX));
        const max_sy = Math.min(this.display_height, Math.ceil(bounds.maxY));


        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                const base_logical_pixel = this.screenToLogicalBase(screen_pixel_center_x, screen_pixel_center_y, itemState);
                const asset_local_x = base_logical_pixel.x - lx_asset_origin;
                const asset_local_y = base_logical_pixel.y - ly_asset_origin;

                if (asset_local_x >= 0 && asset_local_x < assetData.width &&
                    asset_local_y >= 0 && asset_local_y < assetData.height) {

                    const asset_ix = Math.floor(asset_local_x);
                    const asset_iy = Math.floor(asset_local_y);
                    const asset_data_index = asset_iy * assetData.width + asset_ix;

                    if (asset_data_index >= 0 && asset_data_index < assetData.data.length && assetData.data[asset_data_index] === 1) {
                        // Check pixel locking before drawing (only for non-transparent parts of the asset)
                        if (this.pixelOccupationMap) {
                            if (this.isPixelOccupied(sx_iter, sy_iter)) {
                                this.overdrawSkippedPixels++;
                                continue; // Skip already occupied pixels
                            }
                            this.markPixelOccupied(sx_iter, sy_iter, itemState.color);
                        }
                        
                        if (this.optimizationConfig && this.optimizationConfig.enablePixelBatching && this.optimizationAPI && this.optimizationAPI.batchPixelOperations) {
                            this.optimizationAPI.batchPixelOperations(sx_iter, sy_iter, itemState.color);
                        } else {
                            this.ctx.fillStyle = itemState.color;
                            this.ctx.fillRect(sx_iter, sy_iter, 1, 1);
                        }
                    }
                }
            }
        }
    }
}