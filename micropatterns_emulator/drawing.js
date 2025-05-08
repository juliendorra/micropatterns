class MicroPatternsDrawing {
    constructor(ctx) {
        this.ctx = ctx;
        this.display_width = ctx.canvas.width;
        this.display_height = ctx.canvas.height;
        // sinTable and cosTable are no longer needed with DOMMatrix
    }

    // Internal Bresenham's line algorithm operating on SCREEN coordinates.
    // Draws individual 1x1 screen pixels for the line.
    _rawLine(sx1, sy1, sx2, sy2, state) {
        let x1 = Math.trunc(sx1);
        let y1 = Math.trunc(sy1);
        const x2 = Math.trunc(sx2);
        const y2 = Math.trunc(sy2);

        const dx = Math.abs(x2 - x1);
        const dy = -Math.abs(y2 - y1);
        const stepX = x1 < x2 ? 1 : -1;
        const stepY = y1 < y2 ? 1 : -1;
        let err = dx + dy;

        this.ctx.fillStyle = state.color;

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

    // Apply transformations: 1. state.scale, then 2. state.matrix (DOMMatrix)
    // Returns transformed { x, y } screen coordinates (float).
    transformPoint(logicalX, logicalY, state) {
        const scaledX = logicalX * state.scale;
        const scaledY = logicalY * state.scale;

        // DOMMatrix.transformPoint takes a DOMPointInit {x, y, z, w}
        // and returns a DOMPoint.
        const screenPoint = state.matrix.transformPoint({ x: scaledX, y: scaledY });
        return { x: screenPoint.x, y: screenPoint.y };
    }

    // Inverse transform: screen coordinates to base logical coordinates (pattern space)
    // 1. Apply state.inverseMatrix, then 2. Undo state.scale.
    // Returns { x, y } base logical coordinates (float).
    screenToLogicalBase(screenX, screenY, state) {
        // Transform screen point using inverse matrix to get scaled logical coordinates
        const scaledLogicalPoint = state.inverseMatrix.transformPoint({ x: screenX, y: screenY });

        let baseLogicalX = scaledLogicalPoint.x;
        let baseLogicalY = scaledLogicalPoint.y;

        // Undo scaling
        if (state.scale !== 0) {
            baseLogicalX /= state.scale;
            baseLogicalY /= state.scale;
        }
        return { x: baseLogicalX, y: baseLogicalY };
    }


    // Helper function to get the correct pixel color for fills.
    // screenPixelCenterX, screenPixelCenterY are canvas coordinates of the pixel center.
    // Returns the effective color ('black', 'white') or 'white' if the pattern pixel is off.
    _getFillAssetPixelColor(screenPixelCenterX, screenPixelCenterY, state) {
        const fillAsset = state.fillAsset;
        const stateColor = state.color;

        if (!fillAsset) {
            return stateColor; // Solid fill
        }

        // Get base logical coordinates for pattern sampling
        const baseCoords = this.screenToLogicalBase(screenPixelCenterX, screenPixelCenterY, state);
        
        let logicalX = baseCoords.x;
        let logicalY = baseCoords.y;

        // Tiling logic using logical coordinates
        // Ensure integer coordinates for array indexing, and handle negative modulo
        let assetX = Math.floor(logicalX) % fillAsset.width;
        let assetY = Math.floor(logicalY) % fillAsset.height;
        
        if (assetX < 0) assetX += fillAsset.width;
        if (assetY < 0) assetY += fillAsset.height;

        const index = assetY * fillAsset.width + assetX;

        if (index < 0 || index >= fillAsset.data.length) {
            // This should ideally not happen with correct modulo.
            // console.warn(`Fill asset index out of bounds: (${assetX}, ${assetY}) for screen (${screenPixelCenterX}, ${screenPixelCenterY}) from logical (${logicalX}, ${logicalY})`);
            return 'white'; // Default to white/transparent on error
        }
        return fillAsset.data[index] === 1 ? stateColor : 'white';
    }

    // Helper function to set a single "logical pixel" block on the canvas, considering scale.
    // screenX, screenY are the top-left coordinates of the block on the canvas.
    // This is used by DRAW_PIXEL and DRAW_FILLED_PIXEL.
    setPixel(screenX, screenY, color, state) {
        const scale = Math.round(state.scale); // Use rounded scale
        this.ctx.fillStyle = color;
        this.ctx.fillRect(Math.trunc(screenX), Math.trunc(screenY), scale, scale);
    }

    // --- Drawing Primitives ---

    drawLine(x1, y1, x2, y2, state) {
        const p1 = this.transformPoint(x1, y1, state);
        const p2 = this.transformPoint(x2, y2, state);
        this._rawLine(p1.x, p1.y, p2.x, p2.y, state);
    }

    drawRect(x, y, width, height, state) {
        const p1 = this.transformPoint(x, y, state);
        const p2 = this.transformPoint(x + width -1, y, state);
        const p3 = this.transformPoint(x + width -1, y + height -1, state);
        const p4 = this.transformPoint(x, y + height -1, state);

        this._rawLine(p1.x, p1.y, p2.x, p2.y, state);
        this._rawLine(p2.x, p2.y, p3.x, p3.y, state);
        this._rawLine(p3.x, p3.y, p4.x, p4.y, state);
        this._rawLine(p4.x, p4.y, p1.x, p1.y, state);
    }
    
    fillRect(lx, ly, lw, lh, state) {
        if (lw <= 0 || lh <= 0) return;

        // 1. Transform logical corners to screen space
        const s_tl = this.transformPoint(lx, ly, state);
        const s_tr = this.transformPoint(lx + lw, ly, state);
        const s_br = this.transformPoint(lx + lw, ly + lh, state);
        const s_bl = this.transformPoint(lx, ly + lh, state);

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

        // 3. Iterate over screen pixels in this bounding box
        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                // For each screen pixel, inverse transform its center to scaled logical space
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                
                const scaled_logical_pixel = state.inverseMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });

                // Define the rectangle in scaled logical coordinates
                const sl_rect_x = lx * state.scale;
                const sl_rect_y = ly * state.scale;
                const sl_rect_w = lw * state.scale;
                const sl_rect_h = lh * state.scale;

                // Check if (scaled_logical_pixel.x, scaled_logical_pixel.y) is within the scaled logical rectangle
                if (scaled_logical_pixel.x >= sl_rect_x && scaled_logical_pixel.x < (sl_rect_x + sl_rect_w) &&
                    scaled_logical_pixel.y >= sl_rect_y && scaled_logical_pixel.y < (sl_rect_y + sl_rect_h)) {
                    
                    const fillColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, state);
                    if (fillColor !== 'white') {
                        this.ctx.fillStyle = fillColor;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1); // Draw 1x1 screen pixel
                    }
                }
            }
        }
    }

    drawPixel(x, y, state) {
        const p = this.transformPoint(x, y, state);
        this.setPixel(p.x, p.y, state.color, state);
    }

    drawCircle(cx, cy, radius, state) {
        const center = this.transformPoint(cx, cy, state);
        
        // Estimate screen radius. This is an approximation if matrix has non-uniform scaling.
        // For a simple scaled radius:
        const p_edge = this.transformPoint(cx + radius, cy, state);
        const scaledRadius = Math.hypot(p_edge.x - center.x, p_edge.y - center.y);
        const screenRadius = Math.max(1, Math.trunc(scaledRadius));

        let x = screenRadius;
        let y = 0;
        let err = 1 - screenRadius;
        
        this.ctx.fillStyle = state.color;

        while (x >= y) {
            const points = [
                { dx: x, dy: y }, { dx: y, dy: x },
                { dx: -y, dy: x }, { dx: -x, dy: y },
                { dx: -x, dy: -y }, { dx: -y, dy: -x },
                { dx: y, dy: -x }, { dx: x, dy: -y }
            ];

            for (const pt of points) {
                const sx = Math.trunc(center.x + pt.dx);
                const sy = Math.trunc(center.y + pt.dy);
                if (sx >= 0 && sx < this.display_width && sy >= 0 && sy < this.display_height) {
                     this.ctx.fillRect(sx, sy, 1, 1); // Draw 1x1 screen pixel for outline
                }
            }

            y++;
            if (err <= 0) {
                err += 2 * y + 1;
            } else {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }

    fillCircle(lcx, lcy, lr, state) {
        if (lr <= 0) return;

        // 1. Transform logical center to screen space
        const screen_center = this.transformPoint(lcx, lcy, state);

        // 2. Estimate screen radius for bounding box.
        // Transform a point on the logical circle's edge to screen space.
        const logical_edge_x = lcx + lr; // A point on the logical circle edge
        const logical_edge_y = lcy;
        const screen_edge = this.transformPoint(logical_edge_x, logical_edge_y, state);
        
        // Calculate distance from screen_center to screen_edge as an approximate screen radius.
        // This handles uniform scaling and rotation correctly. For non-uniform scaling, it's an axis of the ellipse.
        let approx_screen_radius = Math.hypot(screen_edge.x - screen_center.x, screen_edge.y - screen_center.y);
        
        // If matrix has significant shear or non-uniform scale, we might need to check other points or use a more complex bounding box.
        // For simplicity, we'll use this, but a more robust method would be to transform 4 points (lcx +/- lr, lcy +/- lr) and find their screen bounds.
        // Let's refine by checking another axis for a slightly better bounding box for ellipses.
        const logical_edge_y_alt = this.transformPoint(lcx, lcy + lr, state);
        const approx_screen_radius_y = Math.hypot(logical_edge_y_alt.x - screen_center.x, logical_edge_y_alt.y - screen_center.y);
        approx_screen_radius = Math.max(approx_screen_radius, approx_screen_radius_y, 1.0);


        // 3. Screen bounding box
        let min_sx = Math.trunc(screen_center.x - approx_screen_radius);
        let max_sx = Math.ceil(screen_center.x + approx_screen_radius);
        let min_sy = Math.trunc(screen_center.y - approx_screen_radius);
        let max_sy = Math.ceil(screen_center.y + approx_screen_radius);

        // Clip to canvas
        min_sx = Math.max(0, min_sx);
        min_sy = Math.max(0, min_sy);
        max_sx = Math.min(this.display_width, max_sx);
        max_sy = Math.min(this.display_height, max_sy);

        const scaled_logical_radius_sq = (lr * state.scale) * (lr * state.scale);
        const scaled_logical_cx = lcx * state.scale;
        const scaled_logical_cy = lcy * state.scale;

        // 4. Iterate screen pixels
        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;

                // Transform screen pixel center back to scaled logical space for distance check
                const scaled_logical_pixel = state.inverseMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });
                
                const dist_sq = (scaled_logical_pixel.x - scaled_logical_cx) * (scaled_logical_pixel.x - scaled_logical_cx) +
                                (scaled_logical_pixel.y - scaled_logical_cy) * (scaled_logical_pixel.y - scaled_logical_cy);

                if (dist_sq <= scaled_logical_radius_sq) {
                    const fillColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, state);
                    if (fillColor !== 'white') {
                        this.ctx.fillStyle = fillColor;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1); // Draw 1x1 screen pixel
                    }
                }
            }
        }
    }

    drawFilledPixel(x, y, state) {
        const p_top_left_screen = this.transformPoint(x, y, state);
        // For a single logical pixel, we determine its screen footprint.
        // The "block" is defined by transforming (x,y) and (x+1, y+1)
        const p_bottom_right_screen = this.transformPoint(x + 1, y + 1, state);

        const min_sx = Math.trunc(Math.min(p_top_left_screen.x, p_bottom_right_screen.x));
        const max_sx = Math.ceil(Math.max(p_top_left_screen.x, p_bottom_right_screen.x));
        const min_sy = Math.trunc(Math.min(p_top_left_screen.y, p_bottom_right_screen.y));
        const max_sy = Math.ceil(Math.max(p_top_left_screen.y, p_bottom_right_screen.y));
        
        const scaled_lx = x * state.scale;
        const scaled_ly = y * state.scale;
        const next_scaled_lx = (x + 1) * state.scale;
        const next_scaled_ly = (y + 1) * state.scale;

        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            if (sy_iter < 0 || sy_iter >= this.display_height) continue;
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                if (sx_iter < 0 || sx_iter >= this.display_width) continue;

                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;
                
                const slp = state.inverseMatrix.transformPoint({ x: screen_pixel_center_x, y: screen_pixel_center_y });

                if (slp.x >= scaled_lx && slp.x < next_scaled_lx &&
                    slp.y >= scaled_ly && slp.y < next_scaled_ly) {
                    
                    const effectiveColor = this._getFillAssetPixelColor(screen_pixel_center_x, screen_pixel_center_y, state);
                    if (effectiveColor !== 'white') {
                        this.ctx.fillStyle = effectiveColor;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1); // Draw 1x1 screen pixel
                    }
                }
            }
        }
    }

    drawAsset(lx_asset_origin, ly_asset_origin, assetData, state) {
        if (assetData.width <= 0 || assetData.height <= 0) return;

        // 1. Transform asset's logical bounding box corners to screen space
        const s_tl = this.transformPoint(lx_asset_origin, ly_asset_origin, state);
        const s_tr = this.transformPoint(lx_asset_origin + assetData.width, ly_asset_origin, state);
        const s_br = this.transformPoint(lx_asset_origin + assetData.width, ly_asset_origin + assetData.height, state);
        const s_bl = this.transformPoint(lx_asset_origin, ly_asset_origin + assetData.height, state);

        // 2. Screen bounding box
        let min_sx = Math.min(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let max_sx = Math.max(s_tl.x, s_tr.x, s_br.x, s_bl.x);
        let min_sy = Math.min(s_tl.y, s_tr.y, s_br.y, s_bl.y);
        let max_sy = Math.max(s_tl.y, s_tr.y, s_br.y, s_bl.y);
        
        min_sx = Math.max(0, Math.trunc(min_sx));
        min_sy = Math.max(0, Math.trunc(min_sy));
        max_sx = Math.min(this.display_width, Math.ceil(max_sx));
        max_sy = Math.min(this.display_height, Math.ceil(max_sy));

        // 3. Iterate screen pixels
        for (let sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
            for (let sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
                const screen_pixel_center_x = sx_iter + 0.5;
                const screen_pixel_center_y = sy_iter + 0.5;

                // Transform screen pixel center back to base logical space for asset sampling
                const base_logical_pixel = this.screenToLogicalBase(screen_pixel_center_x, screen_pixel_center_y, state);

                // Convert base_logical_pixel to asset's local coordinates
                const asset_local_x = base_logical_pixel.x - lx_asset_origin;
                const asset_local_y = base_logical_pixel.y - ly_asset_origin;

                // Check if inside asset's logical dimensions
                if (asset_local_x >= 0 && asset_local_x < assetData.width &&
                    asset_local_y >= 0 && asset_local_y < assetData.height) {

                    const asset_ix = Math.floor(asset_local_x);
                    const asset_iy = Math.floor(asset_local_y);
                    const asset_data_index = asset_iy * assetData.width + asset_ix;

                    if (asset_data_index >= 0 && asset_data_index < assetData.data.length && assetData.data[asset_data_index] === 1) {
                        this.ctx.fillStyle = state.color;
                        this.ctx.fillRect(sx_iter, sy_iter, 1, 1); // Draw 1x1 screen pixel
                    }
                }
            }
        }
    }
}