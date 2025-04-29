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

    // --- Transformation ---

    // Apply current state transformations (Scale -> Rotate -> Translate)
    // Returns transformed { x, y } which represents the top-left of the potentially scaled pixel/element.
    transformPoint(x, y, state) {
        let curX = x;
        let curY = y;

        // 1. Scale (relative to origin 0,0 before translation)
        // Scaling affects coordinates AND radius/size values passed to drawing functions
        // We apply scale here only to the base coordinates. Size parameters are scaled separately if needed.
        // For pixel-based drawing, this calculates the top-left of the scaled block.
        curX = Math.trunc(curX * state.scale);
        curY = Math.trunc(curY * state.scale);

        // 2. Rotate (around origin 0,0 before translation)
        if (state.rotation !== 0) {
            const angle = state.rotation;
            // Ensure angle is within 0-359 for table lookup
            const normalizedAngle = (angle % 360 + 360) % 360;
            const sinA = this.sinTable[normalizedAngle];
            const cosA = this.cosTable[normalizedAngle];
            const rotatedX = Math.trunc((curX * cosA - curY * sinA) / 256);
            const rotatedY = Math.trunc((curX * sinA + curY * cosA) / 256);
            curX = rotatedX;
            curY = rotatedY;
        }

        // 3. Translate
        curX += state.translateX;
        curY += state.translateY;

        return { x: curX, y: curY };
    }

    // --- Drawing Primitives ---

    // Internal function to set a single logical pixel on the canvas,
    // drawing a scale x scale block respecting clipping.
    // Accepts the calculated top-left screen coordinates (x, y) and the drawing state.
    setPixel(x, y, color, state) {
        const scale = state.scale;
        // Calculate the integer floor for the top-left corner on the canvas
        const startX = Math.floor(x);
        const startY = Math.floor(y);

        // Check if the *entire* scaled block is outside the canvas bounds
        if (startX >= this.display_width || startY >= this.display_height ||
            startX + scale <= 0 || startY + scale <= 0) {
            return; // Block is completely off-screen
        }

        // Clip the drawing rectangle to the canvas bounds
        const clipX = Math.max(0, startX);
        const clipY = Math.max(0, startY);
        const clipMaxX = Math.min(this.display_width, startX + scale);
        const clipMaxY = Math.min(this.display_height, startY + scale);
        const clipW = clipMaxX - clipX;
        const clipH = clipMaxY - clipY;

        // Only draw if the clipped dimensions are valid
        if (clipW > 0 && clipH > 0) {
            this.ctx.fillStyle = color;
            this.ctx.fillRect(clipX, clipY, clipW, clipH);
        }
    }

     // Internal function to get fill asset pixel color (returns 'white' or state.color)
     // Renamed from getPatternPixel
     getFillAssetPixel(screenX, screenY, fillAsset, stateColor) {
         if (!fillAsset) {
             return stateColor; // Solid fill with the current state color
         }
         // Use Math.floor for consistency with canvas coordinates
         const assetX = Math.floor(screenX) % fillAsset.width;
         const assetY = Math.floor(screenY) % fillAsset.height;
         // Handle negative modulo results
         const px = (assetX < 0) ? assetX + fillAsset.width : assetX;
         const py = (assetY < 0) ? assetY + fillAsset.height : assetY;

         const index = py * fillAsset.width + px;
         if (index < 0 || index >= fillAsset.data.length) {
             console.warn(`Asset index out of bounds: (${px}, ${py}) for screen (${screenX}, ${screenY})`);
             return 'white'; // Default to white/transparent on error
         }
         // Return the state color only if the asset bit is 1, otherwise white
         return fillAsset.data[index] === 1 ? stateColor : 'white';
     }


    drawPixel(x, y, state) {
        // Transform the logical coordinate (x, y) to get the top-left of the block
        const p = this.transformPoint(x, y, state);
        // Set the pixel using the state color and scale
        this.setPixel(p.x, p.y, state.color, state);
    }

    // Bresenham's line algorithm (integer math)
    // Draws a line of potentially scaled pixels.
    drawLine(x1, y1, x2, y2, state) {
        // Transform the logical start and end points
        const p1 = this.transformPoint(x1, y1, state);
        const p2 = this.transformPoint(x2, y2, state);

        // Use the transformed points for Bresenham's algorithm
        // Note: Bresenham calculates the top-left corners of the blocks to draw.
        let sx = Math.trunc(p1.x);
        let sy = Math.trunc(p1.y);
        const ex = Math.trunc(p2.x);
        const ey = Math.trunc(p2.y);

        const dx = Math.abs(ex - sx);
        const dy = -Math.abs(ey - sy);
        const stepX = sx < ex ? 1 : -1;
        const stepY = sy < ey ? 1 : -1;
        let err = dx + dy;

        // Adjust step size based on scale for Bresenham? No, Bresenham calculates
        // the *centers* or *corners* of the pixels to light up. We draw blocks
        // at these calculated positions. The transformPoint already handles scaling
        // the endpoints.

        while (true) {
            // Draw a scaled block at the current Bresenham position (sx, sy)
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

    drawRect(x, y, width, height, state) {
        // Rect dimensions are NOT scaled/rotated by transform, only the top-left corner is.
        // Draw 4 lines respecting the current state color and scale.

        // Transform the 4 corners of the logical rectangle
        const p1 = this.transformPoint(x, y, state); // Top-left
        const p2 = this.transformPoint(x + width -1, y, state); // Top-right (use width-1 for pixel coords)
        const p3 = this.transformPoint(x + width -1, y + height -1, state); // Bottom-right
        const p4 = this.transformPoint(x, y + height -1, state); // Bottom-left

        // Draw lines between the transformed corners. drawLine handles scaling.
        this.drawLine(x, y, x + width -1, y, state); // Top (use logical coords for drawLine)
        this.drawLine(x + width -1, y, x + width -1, y + height -1, state); // Right
        this.drawLine(x + width -1, y + height -1, x, y + height -1, state); // Bottom
        this.drawLine(x, y + height -1, x, y, state); // Left
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

                // Determine the color based on the fill asset using the renamed function
                // Use the transformed screen coordinate (p.x, p.y) for asset lookup
                const effectiveColor = this.getFillAssetPixel(p.x, p.y, state.fillAsset, state.color);

                // Draw the scaled block if the color is not white
                if (effectiveColor !== 'white') {
                    this.setPixel(p.x, p.y, effectiveColor, state);
                }
            }
        }
    }


    // Midpoint circle algorithm (integer math) for outline
    drawCircle(cx, cy, radius, state) {
        // Transform the logical center point
        const center = this.transformPoint(cx, cy, state);
        // Scale the radius appropriately. Use Math.max to ensure radius >= 1.
        // We draw blocks, so the radius effectively refers to the center of the blocks.
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
        // Transform the logical center point
        const center = this.transformPoint(cx, cy, state);
        // Scale the radius. Add 0.5 * scale to check against the edge of the scaled blocks.
        const scaledRadius = Math.max(1, radius * state.scale);
        const rSquared = scaledRadius * scaledRadius;

        // Calculate bounding box based on scaled radius and transformed center
        const minX = Math.floor(center.x - scaledRadius);
        const minY = Math.floor(center.y - scaledRadius);
        const maxX = Math.ceil(center.x + scaledRadius);
        const maxY = Math.ceil(center.y + scaledRadius);

        // Iterate through screen pixels within the bounding box
        for (let screenY = minY; screenY < maxY; screenY += state.scale) {
            for (let screenX = minX; screenX < maxX; screenX += state.scale) {
                // Check if the *center* of the potential scaled block is inside the circle
                const blockCenterX = screenX + state.scale / 2;
                const blockCenterY = screenY + state.scale / 2;
                const dx = blockCenterX - center.x;
                const dy = blockCenterY - center.y;

                if (dx * dx + dy * dy <= rSquared) {
                    // Get fill asset color for the top-left of the block using renamed function
                    const effectiveColor = this.getFillAssetPixel(screenX, screenY, state.fillAsset, state.color);
                     if (effectiveColor !== 'white') {
                         // Draw the scaled block
                         this.setPixel(screenX, screenY, effectiveColor, state);
                     }
                }
            }
        }
    }

    // Renamed from drawIcon - draws a defined pattern directly
    drawAsset(x, y, assetData, state) {
        // Asset's logical top-left (x,y) is transformed first.
        const origin = this.transformPoint(x, y, state);

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