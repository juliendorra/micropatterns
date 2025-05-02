class MicroPatternsDrawing {
    constructor(ctx) {
        this.ctx = ctx;
        this.display_width = ctx.canvas.width;
        this.display_height = ctx.canvas.height;

        // Precompute sin/cos tables (scaled by 256 for integer math simulation)
        // These are used for rotation calculations.
        this.sinTable = [];
        this.cosTable = [];
        for (let i = 0; i < 360; i++) {
            const angleRad = i * Math.PI / 180;
            this.sinTable[i] = Math.floor(Math.sin(angleRad) * 256);
            this.cosTable[i] = Math.floor(Math.cos(angleRad) * 256);
        }
    }

    // Internal Bresenham's line algorithm operating on SCREEN coordinates.
    // Draws scaled pixels between (sx1, sy1) and (sx2, sy2).
    _rawLine(sx1, sy1, sx2, sy2, state) {
        // Use the screen coordinates directly for Bresenham's algorithm
        let sx = Math.trunc(sx1);
        let sy = Math.trunc(sy1);
        const ex = Math.trunc(sx2);
        const ey = Math.trunc(sy2);

        const dx = Math.abs(ex - sx);
        const dy = -Math.abs(ey - sy);
        const stepX = sx < ex ? 1 : -1;
        const stepY = sy < ey ? 1 : -1;
        let err = dx + dy;

        while (true) {
            // Draw a scaled block at the current Bresenham screen position (sx, sy)
            // Use state.color directly as this is the final drawing step for the line.
            this.setPixel(sx, sy, state.color, state);

            if (sx === ex && sy === ey) break; // Reached end point

            const e2 = 2 * err;
            let movedX = false;
            let movedY = false;

            if (e2 >= dy) { // Favor horizontal step
                if (sx === ex) break; // Prevent overshoot if already at target x
                err += dy;
                sx += stepX;
                movedX = true;
            }
            if (e2 <= dx) { // Favor vertical step
                if (sy === ey) break; // Prevent overshoot if already at target y
                err += dx;
                sy += stepY;

                movedY = true;
            }
            // Safety break to prevent infinite loops in edge cases
             if (!movedX && !movedY) break;
        }
    }

    // --- Transformation ---

    // Apply transformations based on the current state, respecting order.
    // 1. Applies the final absolute scale factor (state.scale) relative to (0,0).
    // 2. Applies the sequence of translate and rotate operations stored in state.transformations.
    //    - ROTATE updates the current angle and rotates the point around the current origin offset.
    //    - TRANSLATE calculates global displacement based on the current angle and applies it
    //      to the point and the origin offset.
    // Returns transformed { x, y } screen coordinates (truncated).
    transformPoint(x, y, state) {
        // 1. Apply final scale factor first (relative to original 0,0)
        const finalScale = state.scale;
        // Use integer math as much as possible per spec.
        let globalX = Math.trunc(x * finalScale);
        let globalY = Math.trunc(y * finalScale);

        // Track the accumulated rotation and the origin's offset from global (0,0) due to translations
        let currentAngle = 0; // Angle in degrees (0-359)
        let originOffsetX = 0; // Global X offset of the current origin
        let originOffsetY = 0; // Global Y offset of the current origin

        // 2. Apply Translate and Rotate sequentially
        for (const transform of state.transformations) {
            if (transform.type === 'rotate') {
                const degrees = transform.degrees; // Angle for this specific rotation step

                // Rotate the current point around the current origin offset
                const relX = globalX - originOffsetX;
                const relY = globalY - originOffsetY;

                // Use precomputed tables for the rotation step's angle
                const sinD = this.sinTable[degrees]; // Scaled by 256
                const cosD = this.cosTable[degrees]; // Scaled by 256

                const rotatedRelX = Math.trunc((relX * cosD - relY * sinD) / 256);
                const rotatedRelY = Math.trunc((relX * sinD + relY * cosD) / 256);

                // Update the global point position
                globalX = rotatedRelX + originOffsetX;
                globalY = rotatedRelY + originOffsetY;

                // Update the total accumulated angle
                currentAngle = (currentAngle + degrees) % 360;
                if (currentAngle < 0) currentAngle += 360; // Ensure 0-359 range

            } else if (transform.type === 'translate') {
                const dx = transform.dx; // Translation along current (rotated) X axis
                const dy = transform.dy; // Translation along current (rotated) Y axis

                // Calculate the global displacement based on the *accumulated* currentAngle
                const sinA = this.sinTable[currentAngle]; // Scaled by 256
                const cosA = this.cosTable[currentAngle]; // Scaled by 256

                // Calculate global delta: dx along current X + dy along current Y
                const globalDX = Math.trunc((dx * cosA - dy * sinA) / 256);
                const globalDY = Math.trunc((dx * sinA + dy * cosA) / 256);

                // Shift the point globally
                globalX += globalDX;
                globalY += globalDY;

                // Update the origin's global offset
                originOffsetX += globalDX;
                originOffsetY += globalDY;
            }
            // 'scale' operations are not in the array, only the final state.scale is used (step 1).
        }

        // Return final truncated screen coordinates
        return { x: Math.trunc(globalX), y: Math.trunc(globalY) };
    }


    // --- Drawing Primitives ---

    // Draws a line between logical coordinates (x1, y1) and (x2, y2).
    // Transforms the points and then calls _rawLine.
    drawLine(x1, y1, x2, y2, state) {
        // Transform the logical start and end points to screen coordinates
        const p1 = this.transformPoint(x1, y1, state);
        const p2 = this.transformPoint(x2, y2, state);
        // Draw the line between the transformed screen coordinates
        this._rawLine(p1.x, p1.y, p2.x, p2.y, state);
    }
    // --- Drawing Primitives ---
    // Draws the outline of a rectangle defined by logical coordinates.
    // Transforms the corners and draws lines between the *transformed* screen coordinates.
    drawRect(x, y, width, height, state) {
        // Transform the 4 corners of the logical rectangle to screen coordinates
        const p1 = this.transformPoint(x, y, state); // Top-left
        const p2 = this.transformPoint(x + width - 1, y, state); // Top-right
        const p3 = this.transformPoint(x + width - 1, y + height - 1, state); // Bottom-right
        const p4 = this.transformPoint(x, y + height - 1, state); // Bottom-left
        const startY = Math.floor(y);
        // Draw lines between the transformed screen coordinates using _rawLine
        this._rawLine(p1.x, p1.y, p2.x, p2.y, state); // Top
        this._rawLine(p2.x, p2.y, p3.x, p3.y, state); // Right
        this._rawLine(p3.x, p3.y, p4.x, p4.y, state); // Bottom
        this._rawLine(p4.x, p4.y, p1.x, p1.y, state); // Left
    }

    // Note: fillRect is defined below. The duplicate getFillAssetPixel function definition is removed.
    // Removed extra closing brace here that was causing SyntaxError

    // Helper function to get the correct pixel color for fills, considering fill asset and tiling.
    // screenX, screenY are the top-left canvas coordinates of the pixel/block being considered.
    // state contains fillAsset, color, and scale.
    // Returns the effective color ('black', 'white') or 'white' if the pattern pixel is off.
    _getFillAssetPixelColor(screenX, screenY, state) {
        const fillAsset = state.fillAsset;
        const stateColor = state.color;

        if (!fillAsset) {
            return stateColor; // Solid fill with the current state color
        }

        // Apply scaling to pattern coordinates (matches C++ implementation)
        // Divide screen coordinates by scale factor to get the corresponding pattern coordinate
        // This makes the pattern appear larger when scale factor increases
        let scale = Math.round(state.scale); // Use rounded scale to match C++ implementation
        if (scale < 1) scale = 1;

        // IMPORTANT: We need to apply inverse transformations to get the correct pattern coordinates
        // This ensures pattern rotation works for both FILL_RECT and FILL_CIRCLE
        
        // First, calculate the logical coordinates by applying inverse transformations
        // We need to work backwards from screen coordinates to get the logical coordinates
        // that would map to this screen position
        
        // Start with the current screen position
        let logicalX = screenX;
        let logicalY = screenY;
        
        // Apply inverse transformations in reverse order
        // For each transformation in state.transformations (in reverse)
        if (state.transformations.length > 0) {
            // Track the accumulated rotation and origin offset (in reverse)
            let currentAngle = 0;
            let originOffsetX = 0;
            let originOffsetY = 0;
            
            // Process transformations in forward order to calculate total angle and offset
            for (const transform of state.transformations) {
                if (transform.type === 'rotate') {
                    currentAngle = (currentAngle + transform.degrees) % 360;
                    if (currentAngle < 0) currentAngle += 360;
                } else if (transform.type === 'translate') {
                    // Calculate global displacement based on current angle
                    const sinA = this.sinTable[currentAngle]; // Scaled by 256
                    const cosA = this.cosTable[currentAngle]; // Scaled by 256
                    const globalDX = Math.trunc((transform.dx * cosA - transform.dy * sinA) / 256);
                    const globalDY = Math.trunc((transform.dx * sinA + transform.dy * cosA) / 256);
                    originOffsetX += globalDX;
                    originOffsetY += globalDY;
                }
            }
            
            // Now apply inverse transformations
            // 1. Undo translation
            logicalX -= originOffsetX;
            logicalY -= originOffsetY;
            
            // 2. Undo rotation (apply negative angle)
            if (currentAngle !== 0) {
                const inverseAngle = (360 - currentAngle) % 360;
                const sinD = this.sinTable[inverseAngle]; // Scaled by 256
                const cosD = this.cosTable[inverseAngle]; // Scaled by 256
                
                const rotatedX = Math.trunc((logicalX * cosD - logicalY * sinD) / 256);
                const rotatedY = Math.trunc((logicalX * sinD + logicalY * cosD) / 256);
                
                logicalX = rotatedX;
                logicalY = rotatedY;
            }
        }
        
        // 3. Undo scaling
        logicalX = Math.floor(logicalX / scale);
        logicalY = Math.floor(logicalY / scale);

        // Tiling logic using logical coordinates
        const assetX = logicalX % fillAsset.width;
        const assetY = logicalY % fillAsset.height;
        // Handle negative modulo results
        const px = (assetX < 0) ? assetX + fillAsset.width : assetX;
        const py = (assetY < 0) ? assetY + fillAsset.height : assetY;

        const index = py * fillAsset.width + px;
        if (index < 0 || index >= fillAsset.data.length) {
            console.warn(`Fill asset index out of bounds: (${px}, ${py}) for screen (${screenX}, ${screenY})`);
            return 'white'; // Default to white/transparent on error
        }
        // Return the state color only if the asset bit is 1, otherwise white
        return fillAsset.data[index] === 1 ? stateColor : 'white';
    }


    fillRect(x, y, width, height, state) {
        // Filled rects are complex with rotation and scaling combined with assets.
        // Iterate through the *logical* pixels of the rectangle, transform each,
        // determine the fill asset color, and draw the scaled block.

        for (let ly = 0; ly < height; ly++) {
            for (let lx = 0; lx < width; lx++) {
                // Logical coordinates within the rectangle
                const logicalX = x + lx;
                const logicalY = y + ly;

                // Transform this logical point to screen space (top-left of block)
                const p = this.transformPoint(logicalX, logicalY, state);

                // Determine the color based on the fill asset using the class helper function
                // Pass the whole state object which includes fillAsset, color, and scale
                const effectiveColor = this._getFillAssetPixelColor(p.x, p.y, state);

                // Draw the scaled block if the color is not white
                if (effectiveColor !== 'white') {
                    this.setPixel(p.x, p.y, effectiveColor, state);
                }
            }
        }
    } // <-- Added missing closing brace for fillRect method

    // Helper function to set a single "pixel" block on the canvas, considering scale
    // screenX, screenY are the top-left coordinates of the block on the canvas
    setPixel(screenX, screenY, color, state) {
        const scale = Math.round(state.scale); // Use rounded scale to match C++ implementation
        this.ctx.fillStyle = color;
        // Use Math.trunc to ensure integer coordinates for fillRect
        this.ctx.fillRect(Math.trunc(screenX), Math.trunc(screenY), scale, scale);
    }

    // Draws a single logical pixel, transformed and scaled.
    drawPixel(x, y, state) {
        // Transform the logical coordinate (x, y) to get the top-left of the block
        const p = this.transformPoint(x, y, state);
        // Set the pixel using the state color and scale
        this.setPixel(p.x, p.y, state.color, state);
    }

    // Midpoint circle algorithm (integer math) for outline
    drawCircle(cx, cy, radius, state) {
        // Transform the logical center point to screen coordinates
        const center = this.transformPoint(cx, cy, state);
        // Scale the logical radius by the final absolute scale factor.
        // Use Math.max to ensure radius >= 1 pixel on screen.
        const scaledRadius = Math.max(1, Math.trunc(radius * state.scale));

        let x = scaledRadius;
        let y = 0;
        let err = 1 - scaledRadius;

        // The algorithm calculates offsets (x, y) from the center.
        // We need to draw scaled blocks at these offset positions.
        while (x >= y) {
            // Draw 8 octants using scaled blocks
            this.setPixel(center.x + x, center.y + y, state.color, state);
            this.setPixel(center.x + y, center.y + x, state.color, state);
            this.setPixel(center.x - y, center.y + x, state.color, state);
            this.setPixel(center.x - x, center.y + y, state.color, state);
            this.setPixel(center.x - x, center.y - y, state.color, state);
            this.setPixel(center.x - y, center.y - x, state.color, state);
            this.setPixel(center.x + y, center.y - x, state.color, state);
            this.setPixel(center.x + x, center.y - y, state.color, state);

            y++;
            if (err <= 0) {
                err += 2 * y + 1;
            } else { // err > 0
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }

    // Fill circle using scanlines within the circle's bounds, applying fill assets and scaling
    fillCircle(cx, cy, radius, state) {
        // Transform the logical center point to screen coordinates
        const center = this.transformPoint(cx, cy, state);
        // Scale the logical radius by the final absolute scale factor.
        // Use Math.max to ensure radius >= 1 pixel on screen.
        const scaledRadius = Math.max(1, radius * state.scale);
        // Use square of radius for distance check (avoids sqrt)
        const rSquared = scaledRadius * scaledRadius;

        // Calculate bounding box based on scaled radius and transformed center
        // Match C++ implementation by using floor/ceil for min/max
        const minX = Math.floor(center.x - scaledRadius);
        const minY = Math.floor(center.y - scaledRadius);
        const maxX = Math.ceil(center.x + scaledRadius);
        const maxY = Math.ceil(center.y + scaledRadius);

        // Clip bounding box to canvas (match C++ implementation)
        const clippedMinX = Math.max(0, minX);
        const clippedMinY = Math.max(0, minY);
        const clippedMaxX = Math.min(this.display_width, maxX);
        const clippedMaxY = Math.min(this.display_height, maxY);

        // Iterate through individual pixels within the bounding box (match C++ implementation)
        // Don't step by scale - check every pixel like C++ does
        for (let sy = clippedMinY; sy < clippedMaxY; sy++) {
            for (let sx = clippedMinX; sx < clippedMaxX; sx++) {
                // Check if the center of the pixel is inside the circle (match C++ implementation)
                const dx = (sx + 0.5) - center.x;
                const dy = (sy + 0.5) - center.y;

                if (dx * dx + dy * dy <= rSquared) {
                    // Get fill color for this screen pixel
                    const fillColor = this._getFillAssetPixelColor(sx, sy, state);
                    if (fillColor !== 'white') {
                        // Draw a single raw pixel (not a scaled block)
                        this.ctx.fillStyle = fillColor;
                        this.ctx.fillRect(sx, sy, 1, 1);
                    }
                }
            }
        }
    }

    // Draws a single logical pixel, conditionally based on the current fill pattern.
    drawFilledPixel(x, y, state) {
        // Transform the logical coordinate (x, y) to get the top-left of the block
        const p = this.transformPoint(x, y, state);

        // Determine the effective color based on the fill asset at the screen coordinate (p.x, p.y)
        // Pass the whole state object which includes fillAsset, color, and scale
        const effectiveColor = this._getFillAssetPixelColor(p.x, p.y, state);

        // Draw the scaled block only if the effective color is not white
        // (i.e., solid fill, or pattern pixel is '1')
        if (effectiveColor !== 'white') {
            this.setPixel(p.x, p.y, effectiveColor, state);
        }
    }

    // Renamed from drawIcon - draws a defined pattern directly
    drawAsset(x, y, assetData, state) {
        // Asset's logical top-left (x,y) is transformed first.
        // Note: The origin calculation here isn't strictly needed as we transform each pixel individually below.
        // const origin = this.transformPoint(x, y, state);

        // Iterate through the asset's logical pixels
        for (let iy = 0; iy < assetData.height; iy++) {
            for (let ix = 0; ix < assetData.width; ix++) {
                const index = iy * assetData.width + ix;
                if (assetData.data[index] === 1) { // If asset pixel is 'on'
                    // Calculate the logical position of this asset pixel relative to the asset's origin (x,y)
                    const logicalPixelX = x + ix;
                    const logicalPixelY = y + iy;

                    // Transform this logical point to screen space (top-left of block)
                    // Note: transformPoint applies scale, rotation, and translation based on the *state*
                    // This means the entire asset is scaled/rotated/translated as a unit based on its origin (x,y)
                    const p = this.transformPoint(logicalPixelX, logicalPixelY, state);

                    // Draw the scaled block using the current state color
                    this.setPixel(p.x, p.y, state.color, state);
                }
            }
        }
    }
}