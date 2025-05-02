#include "micropatterns_drawing.h"
#include <cmath> // For round, floor, ceil in some places, but avoid for core transforms

// Define colors (assuming 0=white, 15=black for M5EPD 4bpp buffer)
const uint8_t COLOR_WHITE = 0;
const uint8_t COLOR_BLACK = 15;

MicroPatternsDrawing::MicroPatternsDrawing(M5EPD_Canvas* canvas) : _canvas(canvas) {
    if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
    precomputeTrigTables();
}

void MicroPatternsDrawing::setCanvas(M5EPD_Canvas* canvas) {
    _canvas = canvas;
     if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
}

void MicroPatternsDrawing::clearCanvas() {
    if (_canvas) {
        _canvas->fillCanvas(COLOR_WHITE);
    }
}

void MicroPatternsDrawing::precomputeTrigTables() {
    _sinTable.resize(360);
    _cosTable.resize(360);
    for (int i = 0; i < 360; ++i) {
        float angleRad = i * DEG_TO_RAD;
        // Store as fixed-point integers scaled by FIXED_POINT_SCALE
        _sinTable[i] = static_cast<int>(round(sin(angleRad) * FIXED_POINT_SCALE));
        _cosTable[i] = static_cast<int>(round(cos(angleRad) * FIXED_POINT_SCALE));
    }
}


// --- Transformation ---
// Applies scale, then the sequence of translate/rotate operations using integer math.
// Converts logical (lx, ly) to screen (sx, sy).
void MicroPatternsDrawing::transformPoint(int lx, int ly, const MicroPatternsState& state, int& sx, int& sy) {
    // 1. Apply final scale factor first (relative to original 0,0)
    // Use float for scale factor itself, but keep coords integer for now.
    // Intermediate calculations might need larger types (int32_t) if scale is large.
    int32_t currentX = round(lx * state.scale);
    int32_t currentY = round(ly * state.scale);

    // Track the accumulated rotation and the origin's offset from global (0,0) due to translations
    int currentAngle = 0; // Angle in degrees (0-359)
    int32_t originOffsetX = 0; // Global X offset of the current origin
    int32_t originOffsetY = 0; // Global Y offset of the current origin

    // 2. Apply Translate and Rotate sequentially from the state's list
    for (const auto& op : state.transformations) {
        if (op.type == TransformOp::ROTATE) {
            int degrees = static_cast<int>(round(op.value1)) % 360;
            if (degrees < 0) degrees += 360;

            // Rotate the current point around the current origin offset (originOffsetX, originOffsetY)
            int32_t relX = currentX - originOffsetX;
            int32_t relY = currentY - originOffsetY;

            // Use precomputed tables (already scaled by FIXED_POINT_SCALE)
            int sinD = _sinTable[degrees];
            int cosD = _cosTable[degrees];

            // Perform rotation using fixed-point multiplication
            // (relX * cosD - relY * sinD) / FIXED_POINT_SCALE
            // (relX * sinD + relY * cosD) / FIXED_POINT_SCALE
            int32_t rotatedRelX = FIXED_TO_INT(relX * cosD - relY * sinD);
            int32_t rotatedRelY = FIXED_TO_INT(relX * sinD + relY * cosD);

            // Update the global point position
            currentX = rotatedRelX + originOffsetX;
            currentY = rotatedRelY + originOffsetY;

            // Update the total accumulated angle for subsequent translations
            currentAngle = (currentAngle + degrees) % 360; // Keep track of angle for translate
            // Note: The spec says ROTATE sets absolute angle. If so, currentAngle should just be set to degrees.
            // Let's assume cumulative rotation based on JS implementation for now.
            // If ROTATE is absolute, change `currentAngle = (currentAngle + degrees) % 360;` to `currentAngle = degrees;`

        } else if (op.type == TransformOp::TRANSLATE) {
            int32_t dx = round(op.value1); // Translation along current (rotated) X axis
            int32_t dy = round(op.value2); // Translation along current (rotated) Y axis

            // Calculate the global displacement based on the *accumulated* currentAngle
            int sinA = _sinTable[currentAngle];
            int cosA = _cosTable[currentAngle];

            // Calculate global delta using fixed-point math:
            // globalDX = (dx * cosA - dy * sinA) / FIXED_POINT_SCALE
            // globalDY = (dx * sinA + dy * cosA) / FIXED_POINT_SCALE
            int32_t globalDX = FIXED_TO_INT(dx * cosA - dy * sinA);
            int32_t globalDY = FIXED_TO_INT(dx * sinA + dy * cosA);

            // Shift the point globally
            currentX += globalDX;
            currentY += globalDY;

            // Update the origin's global offset as well
            originOffsetX += globalDX;
            originOffsetY += globalDY;
        }
    }

    // Final conversion to integer screen coordinates
    sx = static_cast<int>(currentX);
    sy = static_cast<int>(currentY);
}


// --- Raw Drawing ---
// Draws a single pixel on the canvas at screen coordinates
void MicroPatternsDrawing::rawPixel(int sx, int sy, uint8_t color) {
    if (!_canvas) return;
    // Basic bounds check
    if (sx >= 0 && sx < _canvasWidth && sy >= 0 && sy < _canvasHeight) {
        _canvas->drawPixel(sx, sy, color);
    }
}

// Basic Bresenham line algorithm on screen coordinates, calls rawPixel
void MicroPatternsDrawing::rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color) {
    if (!_canvas) return;

    int dx = abs(sx2 - sx1);
    int dy = -abs(sy2 - sy1);
    int sx = sx1;
    int sy = sy1;
    int stepX = sx1 < sx2 ? 1 : -1;
    int stepY = sy1 < sy2 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        rawPixel(sx, sy, color);
        if (sx == sx2 && sy == sy2) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            if (sx == sx2) break;
            err += dy;
            sx += stepX;
        }
        if (e2 <= dx) {
            if (sy == sy2) break;
            err += dx;
            sy += stepY;
        }
    }
}

// Helper to draw a scaled block using fillRect for efficiency
void MicroPatternsDrawing::drawScaledBlock(int screenX, int screenY, int scale, uint8_t color) {
     if (!_canvas || scale <= 0) return;
     // Ensure scale is at least 1
     int drawScale = (scale < 1) ? 1 : scale;

     // Basic bounds check for the rectangle
     // Check if any part of the rectangle is within bounds
     if (screenX + drawScale > 0 && screenX < _canvasWidth && screenY + drawScale > 0 && screenY < _canvasHeight) {
          _canvas->fillRect(screenX, screenY, drawScale, drawScale, color);
     }
}


// --- Fill Pattern Helper ---
uint8_t MicroPatternsDrawing::getFillColor(int screenX, int screenY, const MicroPatternsState& state) {
    if (!state.fillAsset) {
        // Solid fill
        return state.color;
    } else {
        // Pattern fill
        const MicroPatternsAsset& asset = *state.fillAsset;
        if (asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return COLOR_WHITE; // Invalid asset

        // Tiling logic using screen coordinates
        // Use floorDiv/floorMod style logic for correct negative coordinate handling
        int assetX = screenX % asset.width;
        int assetY = screenY % asset.height;
        if (assetX < 0) assetX += asset.width;
        if (assetY < 0) assetY += asset.height;

        int index = assetY * asset.width + assetX;
        if (index >= 0 && index < asset.data.size()) {
            // Return state color if pattern bit is 1, otherwise white (transparent)
            return asset.data[index] == 1 ? state.color : COLOR_WHITE;
        } else {
            // Should not happen with correct modulo logic, but safety check
            log_e("Fill asset index out of bounds: (%d, %d) -> %d for screen (%d, %d)", assetX, assetY, index, screenX, screenY);
            return COLOR_WHITE;
        }
    }
}


// --- Drawing Primitives ---

// Draws a logical pixel as a scaled block
void MicroPatternsDrawing::drawPixel(int lx, int ly, const MicroPatternsState& state) {
    if (!_canvas) return;
    int sx, sy;
    // Transform the logical coordinate (center of the pixel?)
    // Let's transform the top-left corner for consistency with fillRect etc.
    transformPoint(lx, ly, state, sx, sy);

    // Draw a scaled block at the transformed screen coordinate
    int scale = round(state.scale);
    drawScaledBlock(sx, sy, scale, state.color);
}

void MicroPatternsDrawing::drawLine(int lx1, int ly1, int lx2, int ly2, const MicroPatternsState& state) {
    if (!_canvas) return;
    int sx1, sy1, sx2, sy2;
    transformPoint(lx1, ly1, state, sx1, sy1);
    transformPoint(lx2, ly2, state, sx2, sy2);
    // Draw line between transformed screen points using raw pixels
    rawLine(sx1, sy1, sx2, sy2, state.color);
}

void MicroPatternsDrawing::drawRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state) {
    if (!_canvas || lw <= 0 || lh <= 0) return;
    // Transform corners
    int sx1, sy1, sx2, sy2, sx3, sy3, sx4, sy4;
    // Transform corners relative to logical origin (lx, ly)
    transformPoint(lx, ly, state, sx1, sy1);             // Top-left
    transformPoint(lx + lw -1, ly, state, sx2, sy2);         // Top-right
    transformPoint(lx + lw -1, ly + lh -1, state, sx3, sy3); // Bottom-right
    transformPoint(lx, ly + lh -1, state, sx4, sy4);         // Bottom-left

    // Draw sides using raw pixels between transformed screen coordinates
    rawLine(sx1, sy1, sx2, sy2, state.color); // Top
    rawLine(sx2, sy2, sx3, sy3, state.color); // Right
    rawLine(sx3, sy3, sx4, sy4, state.color); // Bottom
    rawLine(sx4, sy4, sx1, sy1, state.color); // Left
}

// Fills a rectangle by iterating through logical pixels, transforming, and drawing scaled blocks.
void MicroPatternsDrawing::fillRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state) {
    if (!_canvas || lw <= 0 || lh <= 0) return;
    int scale = round(state.scale);
    if (scale < 1) scale = 1;

    // Iterate through logical grid within the rectangle
    for (int iy = 0; iy < lh; ++iy) {
        for (int ix = 0; ix < lw; ++ix) {
            // Transform the top-left corner of this logical pixel
            int sx, sy;
            transformPoint(lx + ix, ly + iy, state, sx, sy);

            // Determine fill color based on the *screen* coordinate (top-left of block)
            uint8_t fillColor = getFillColor(sx, sy, state);

            if (fillColor != COLOR_WHITE) {
                 // Draw the scaled block
                 drawScaledBlock(sx, sy, scale, fillColor);
            }
        }
    }
}

// Draws circle outline using Midpoint algorithm on screen coordinates.
void MicroPatternsDrawing::drawCircle(int lcx, int lcy, int lr, const MicroPatternsState& state) {
     if (!_canvas || lr <= 0) return;
     int scx, scy;
     // Transform the logical center
     transformPoint(lcx, lcy, state, scx, scy);
     // Scale the logical radius
     int scaledRadius = round(lr * state.scale);
     if (scaledRadius < 1) scaledRadius = 1;

     // Basic Midpoint circle algorithm (operating on screen coordinates)
     int x = scaledRadius;
     int y = 0;
     int err = 1 - scaledRadius; // Initial error term

     while (x >= y) {
         // Draw 8 octants using raw pixels
         rawPixel(scx + x, scy + y, state.color);
         rawPixel(scx + y, scy + x, state.color);
         rawPixel(scx - y, scy + x, state.color);
         rawPixel(scx - x, scy + y, state.color);
         rawPixel(scx - x, scy - y, state.color);
         rawPixel(scx - y, scy - x, state.color);
         rawPixel(scx + y, scy - x, state.color);
         rawPixel(scx + x, scy - y, state.color);

         y++;
         if (err <= 0) {
             err += 2 * y + 1;
         } else {
             x--;
             err += 2 * (y - x) + 1;
         }
     }
}

// Fills circle by iterating bounding box on screen, checking distance, uses fill color.
void MicroPatternsDrawing::fillCircle(int lcx, int lcy, int lr, const MicroPatternsState& state) {
    if (!_canvas || lr <= 0) return;
    int scx, scy;
    // Transform logical center
    transformPoint(lcx, lcy, state, scx, scy);
    // Scale logical radius
    float scaledRadius = lr * state.scale;
    if (scaledRadius < 0.5f) return; // Don't draw if radius is too small
    float rSquared = scaledRadius * scaledRadius;

    // Calculate screen bounding box (integer coords)
    int minX = floor(scx - scaledRadius);
    int minY = floor(scy - scaledRadius);
    int maxX = ceil(scx + scaledRadius);
    int maxY = ceil(scy + scaledRadius);

     // Clip bounding box to canvas
     minX = max(0, minX);
     minY = max(0, minY);
     maxX = min(_canvasWidth, maxX);
     maxY = min(_canvasHeight, maxY);

     // Iterate through screen pixels in the bounding box
    for (int sy = minY; sy < maxY; ++sy) {
        for (int sx = minX; sx < maxX; ++sx) {
            // Check if the center of the screen pixel is inside the circle
            float dx = (float)sx + 0.5f - scx;
            float dy = (float)sy + 0.5f - scy;
            if (dx * dx + dy * dy <= rSquared) {
                // Get fill color for this screen pixel
                uint8_t fillColor = getFillColor(sx, sy, state);
                if (fillColor != COLOR_WHITE) {
                    // Draw a single raw pixel
                    rawPixel(sx, sy, fillColor);
                }
            }
        }
    }
}

// Draws a defined pattern asset by transforming each of its 'on' pixels.
void MicroPatternsDrawing::drawAsset(int lx, int ly, const MicroPatternsAsset& asset, const MicroPatternsState& state) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return;

    int scale = round(state.scale);
    if (scale < 1) scale = 1;

    // Iterate through the asset's logical pixels
    for (int iy = 0; iy < asset.height; ++iy) {
        for (int ix = 0; ix < asset.width; ++ix) {
            int index = iy * asset.width + ix;
            if (index < asset.data.size() && asset.data[index] == 1) { // If asset pixel is 'on' (1)
                // Calculate the logical position of this asset pixel relative to the asset's origin (lx, ly)
                int logicalPixelX = lx + ix;
                int logicalPixelY = ly + iy;

                // Transform this logical point's top-left corner to screen space
                int sx, sy;
                transformPoint(logicalPixelX, logicalPixelY, state, sx, sy);

                 // Draw the scaled block using the current state color
                 drawScaledBlock(sx, sy, scale, state.color);
            }
        }
    }
}

// Draws a pixel conditionally based on the fill pattern at the transformed screen location.
void MicroPatternsDrawing::drawFilledPixel(int lx, int ly, const MicroPatternsState& state) {
    if (!_canvas) return;

    int scale = round(state.scale);
    if (scale < 1) scale = 1;

    // Transform the top-left corner of the logical pixel
    int sx, sy;
    transformPoint(lx, ly, state, sx, sy);

    // Determine the effective color based on the fill asset at the screen coordinate (sx, sy)
    uint8_t effectiveColor = getFillColor(sx, sy, state);

    // Draw the scaled block only if the effective color is not white
    // (i.e., solid fill is active, or pattern pixel is '1' at this screen location)
    if (effectiveColor != COLOR_WHITE) {
         // Draw the scaled block using the determined effective color
         drawScaledBlock(sx, sy, scale, effectiveColor);
    }
}