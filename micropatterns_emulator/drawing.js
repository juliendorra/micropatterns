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
    // Returns transformed { x, y }
    transformPoint(x, y, state) {
        let curX = x;
        let curY = y;

        // 1. Scale (relative to origin 0,0 before translation)
        // Scaling affects coordinates AND radius/size values passed to drawing functions
        // We apply scale here only to the base coordinates. Size parameters are scaled separately if needed.
        curX = Math.trunc(curX * state.scale);
        curY = Math.trunc(curY * state.scale);

        // 2. Rotate (around origin 0,0 before translation)
        if (state.rotation !== 0) {
            const angle = state.rotation;
            const sinA = this.sinTable[angle];
            const cosA = this.cosTable[angle];
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

    // Internal function to set a single pixel on the canvas respecting clipping
    setPixel(x, y, color) {
        // Clip to canvas bounds
        if (x >= 0 && x < this.display_width && y >= 0 && y < this.display_height) {
            this.ctx.fillStyle = color;
            this.ctx.fillRect(Math.floor(x), Math.floor(y), 1, 1); // Use floor for canvas coords
        }
    }

     // Internal function to get pattern pixel color (black/white)
     getPatternPixel(screenX, screenY, pattern, stateColor) {
         if (!pattern) {
             return stateColor; // Solid fill
         }
         const patternX = Math.floor(screenX) % pattern.width;
         const patternY = Math.floor(screenY) % pattern.height;
         // Handle negative modulo results if necessary (though screen coords should be positive)
         const px = (patternX < 0) ? patternX + pattern.width : patternX;
         const py = (patternY < 0) ? patternY + pattern.height : patternY;

         const index = py * pattern.width + px;
         if (index < 0 || index >= pattern.data.length) {
             console.warn(`Pattern index out of bounds: (${px}, ${py})`);
             return 'white'; // Default to white/transparent on error
         }
         return pattern.data[index] === 1 ? stateColor : 'white'; // Use state color for '1'
     }


    drawPixel(x, y, state) {
        const p = this.transformPoint(x, y, state);
        this.setPixel(p.x, p.y, state.color);
    }

    // Bresenham's line algorithm (integer math)
    drawLine(x1, y1, x2, y2, state) {
        const p1 = this.transformPoint(x1, y1, state);
        const p2 = this.transformPoint(x2, y2, state);

        let sx = Math.trunc(p1.x);
        let sy = Math.trunc(p1.y);
        const ex = Math.trunc(p2.x);
        const ey = Math.trunc(p2.y);

        const dx = Math.abs(ex - sx);
        const dy = -Math.abs(ey - sy);
        const stepX = sx < ex ? 1 : -1;
        const stepY = sy < ey ? 1 : -1;
        let err = dx + dy;

        while (true) {
            this.setPixel(sx, sy, state.color);
            if (sx === ex && sy === ey) break;
            const e2 = 2 * err;
            if (e2 >= dy) { // >= favors horizontal steps
                err += dy;
                sx += stepX;
            }
            if (e2 <= dx) { // <= favors vertical steps
                err += dx;
                sy += stepY;
            }
        }
    }

    drawRect(x, y, width, height, state) {
        // Rect dimensions are NOT scaled/rotated by transform, only the top-left corner is.
        // Draw 4 lines respecting the current state color.
        const p = this.transformPoint(x, y, state); // Top-left corner

        // Calculate other corners based on transformed top-left and original w/h
        // This is tricky because rotation affects where the other corners land.
        // A simpler approach for non-filled rects is to draw 4 lines between
        // the transformed corners of the original rectangle.

        const p1 = this.transformPoint(x, y, state); // Top-left
        const p2 = this.transformPoint(x + width, y, state); // Top-right
        const p3 = this.transformPoint(x + width, y + height, state); // Bottom-right
        const p4 = this.transformPoint(x, y + height, state); // Bottom-left

        this.drawLine(p1.x, p1.y, p2.x, p2.y, state); // Top
        this.drawLine(p2.x, p2.y, p3.x, p3.y, state); // Right
        this.drawLine(p3.x, p3.y, p4.x, p4.y, state); // Bottom
        this.drawLine(p4.x, p4.y, p1.x, p1.y, state); // Left
    }

    fillRect(x, y, width, height, state) {
        // Filled rects are complex with rotation. We iterate through the screen pixels
        // within the bounding box of the transformed rectangle and check if each
        // screen pixel corresponds to a point inside the *original* rectangle
        // before inverse transformation. Then apply pattern/color.

        // 1. Find the bounding box of the transformed rectangle
        const p1 = this.transformPoint(x, y, state);
        const p2 = this.transformPoint(x + width, y, state);
        const p3 = this.transformPoint(x + width, y + height, state);
        const p4 = this.transformPoint(x, y + height, state);

        const minX = Math.floor(Math.min(p1.x, p2.x, p3.x, p4.x));
        const minY = Math.floor(Math.min(p1.y, p2.y, p3.y, p4.y));
        const maxX = Math.ceil(Math.max(p1.x, p2.x, p3.x, p4.x));
        const maxY = Math.ceil(Math.max(p1.y, p2.y, p3.y, p4.y));

        // 2. Iterate through pixels in the bounding box
        for (let screenY = minY; screenY < maxY; screenY++) {
            for (let screenX = minX; screenX < maxX; screenX++) {
                // 3. Inverse transform the screen pixel back to the rectangle's local space
                // This is complex. A simpler (but less accurate for rotated thin rects)
                // approach for integer-only is difficult.
                // Approximation: Check if the center of the screen pixel, when inverse
                // transformed, falls within the original rect bounds [x, y] to [x+w, y+h].

                // Let's use a simpler fill method for now: fill the bounding box and rely on clipping
                // or later drawing to overwrite. This isn't accurate for rotated patterns.
                // A true implementation would involve scanline filling or inverse transform checks.

                // Simplified approach: Just fill the axis-aligned bounding box with pattern/color
                // This is NOT correct for rotated rectangles but works for unrotated/scaled.
                // TODO: Implement proper rotated rectangle filling if needed.

                // For now, let's assume no rotation for fillRect pattern application
                // and just fill the transformed axis-aligned area.
                if (state.rotation === 0) {
                     const startX = Math.floor(p1.x);
                     const startY = Math.floor(p1.y);
                     // Scaled width/height
                     const scaledWidth = Math.abs(Math.floor(p2.x) - startX);
                     const scaledHeight = Math.abs(Math.floor(p4.y) - startY);

                     for (let iy = 0; iy < scaledHeight; iy++) {
                         for (let ix = 0; ix < scaledWidth; ix++) {
                             const currentScreenX = startX + ix;
                             const currentScreenY = startY + iy;
                             const color = this.getPatternPixel(currentScreenX, currentScreenY, state.pattern, state.color);
                             if (color !== 'white') { // Don't draw white (background)
                                 this.setPixel(currentScreenX, currentScreenY, color);
                             }
                         }
                     }
                     return; // Exit after simplified fill
                } else {
                     // Fallback for rotated: Draw solid color bounding box (inaccurate)
                     // Or better: draw the 4 lines thicker? No, fill is needed.
                     // We need inverse transform or scanline. Skip pattern for rotated rects for now.
                     console.warn("Pattern fill for rotated RECT not fully implemented, drawing solid outline instead.");
                     this.drawRect(x, y, width, height, state); // Draw outline as fallback
                }
            }
        }
    }


    // Midpoint circle algorithm (integer math) for outline
    drawCircle(cx, cy, radius, state) {
        const center = this.transformPoint(cx, cy, state);
        const scaledRadius = Math.max(1, Math.trunc(radius * state.scale)); // Scale radius

        let x = scaledRadius;
        let y = 0;
        let err = 1 - scaledRadius; // Start error term slightly differently

        while (x >= y) {
            this.setPixel(center.x + x, center.y + y, state.color);
            this.setPixel(center.x + y, center.y + x, state.color);
            this.setPixel(center.x - y, center.y + x, state.color);
            this.setPixel(center.x - x, center.y + y, state.color);
            this.setPixel(center.x - x, center.y - y, state.color);
            this.setPixel(center.x - y, center.y - x, state.color);
            this.setPixel(center.x + y, center.y - x, state.color);
            this.setPixel(center.x + x, center.y - y, state.color);

            y++;
            if (err <= 0) {
                err += 2 * y + 1;
            } else { // err > 0
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }

    // Fill circle using scanlines within the circle's bounds
    fillCircle(cx, cy, radius, state) {
        const center = this.transformPoint(cx, cy, state);
        const scaledRadius = Math.max(1, Math.trunc(radius * state.scale));
        const rSquared = scaledRadius * scaledRadius;

        const minX = Math.floor(center.x - scaledRadius);
        const minY = Math.floor(center.y - scaledRadius);
        const maxX = Math.ceil(center.x + scaledRadius);
        const maxY = Math.ceil(center.y + scaledRadius);

        for (let screenY = minY; screenY < maxY; screenY++) {
            for (let screenX = minX; screenX < maxX; screenX++) {
                // Check if the center of the pixel is inside the circle
                const dx = (screenX + 0.5) - center.x;
                const dy = (screenY + 0.5) - center.y;
                if (dx * dx + dy * dy <= rSquared) {
                    const color = this.getPatternPixel(screenX, screenY, state.pattern, state.color);
                     if (color !== 'white') {
                         this.setPixel(screenX, screenY, color);
                     }
                }
            }
        }
    }

    drawIcon(x, y, iconData, state) {
        // Icon's top-left (x,y) is transformed first.
        // Then, each pixel *within* the icon is transformed relative to that point.
        const origin = this.transformPoint(x, y, state);

        for (let iy = 0; iy < iconData.height; iy++) {
            for (let ix = 0; ix < iconData.width; ix++) {
                const index = iy * iconData.width + ix;
                if (iconData.data[index] === 1) {
                    // Transform the icon's local pixel (ix, iy) using the *same* state
                    // but relative to (0,0) as if the icon's corner is the origin,
                    // then add the transformed icon origin.
                    // Note: Scale/Rotate are applied relative to the icon's local (0,0).

                    let iconPixelX = ix;
                    let iconPixelY = iy;

                     // 1. Scale icon pixel relative to icon's (0,0)
                     iconPixelX = Math.trunc(iconPixelX * state.scale);
                     iconPixelY = Math.trunc(iconPixelY * state.scale);

                     // 2. Rotate icon pixel relative to icon's (0,0)
                     if (state.rotation !== 0) {
                         const angle = state.rotation;
                         const sinA = this.sinTable[angle];
                         const cosA = this.cosTable[angle];
                         const rotatedX = Math.trunc((iconPixelX * cosA - iconPixelY * sinA) / 256);
                         const rotatedY = Math.trunc((iconPixelX * sinA + iconPixelY * cosA) / 256);
                         iconPixelX = rotatedX;
                         iconPixelY = rotatedY;
                     }

                     // 3. Translate to the final screen position by adding the transformed origin
                     const finalX = origin.x + iconPixelX;
                     const finalY = origin.y + iconPixelY;

                    this.setPixel(finalX, finalY, state.color);
                }
            }
        }
    }
}