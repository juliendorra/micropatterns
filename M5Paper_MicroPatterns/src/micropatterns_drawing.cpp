#include "micropatterns_drawing.h"
#include <cmath> // For sin, cos, floor, ceil, round

// Define colors (assuming 0=white, 15=black for M5EPD 4bpp buffer)
const uint8_t COLOR_WHITE = 0;
const uint8_t COLOR_BLACK = 15;

MicroPatternsDrawing::MicroPatternsDrawing(M5EPD_Canvas* canvas) : _canvas(canvas) {}

void MicroPatternsDrawing::setCanvas(M5EPD_Canvas* canvas) {
    _canvas = canvas;
}

void MicroPatternsDrawing::clearCanvas() {
    if (_canvas) {
        _canvas->fillCanvas(COLOR_WHITE);
    }
}

// --- Transformation ---
// Simplified: Applies scale, then rotation (around 0,0), then translation.
// Converts logical (lx, ly) to screen (sx, sy).
void MicroPatternsDrawing::transformPoint(float lx, float ly, const MicroPatternsState& state, int& sx, int& sy) {
    // 1. Scale
    float scaledX = lx * state.scale;
    float scaledY = ly * state.scale;

    // 2. Rotate (around logical 0,0 before translation)
    float rotatedX = scaledX;
    float rotatedY = scaledY;
    if (state.rotationDegrees != 0) {
        float angleRad = state.rotationDegrees * DEG_TO_RAD;
        float cosA = cos(angleRad);
        float sinA = sin(angleRad);
        rotatedX = scaledX * cosA - scaledY * sinA;
        rotatedY = scaledX * sinA + scaledY * cosA;
    }

    // 3. Translate
    float finalX = rotatedX + state.translateX;
    float finalY = rotatedY + state.translateY;

    // Convert to integer screen coordinates
    sx = round(finalX);
    sy = round(finalY);
}

// --- Raw Drawing ---
// Draws a single pixel on the canvas at screen coordinates
void MicroPatternsDrawing::rawPixel(int sx, int sy, uint8_t color) {
    if (!_canvas) return;
    // Basic bounds check
    if (sx >= 0 && sx < _canvas->width() && sy >= 0 && sy < _canvas->height()) {
        _canvas->drawPixel(sx, sy, color);
    }
}

// Basic Bresenham line algorithm on screen coordinates
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

// --- Fill Pattern Helper ---
uint8_t MicroPatternsDrawing::getFillColor(int screenX, int screenY, const MicroPatternsState& state) {
    if (!state.fillAsset) {
        // Solid fill
        return state.color;
    } else {
        // Pattern fill
        const MicroPatternsAsset& asset = *state.fillAsset;
        if (asset.width <= 0 || asset.height <= 0) return COLOR_WHITE; // Invalid asset

        // Tiling logic
        int assetX = screenX % asset.width;
        int assetY = screenY % asset.height;
        if (assetX < 0) assetX += asset.width;
        if (assetY < 0) assetY += asset.height;

        int index = assetY * asset.width + assetX;
        if (index >= 0 && index < asset.data.size()) {
            return asset.data[index] == 1 ? state.color : COLOR_WHITE;
        } else {
            return COLOR_WHITE; // Out of bounds
        }
    }
}


// --- Drawing Primitives ---

void MicroPatternsDrawing::drawPixel(float x, float y, const MicroPatternsState& state) {
    if (!_canvas) return;
    int sx, sy;
    transformPoint(x, y, state, sx, sy);
    // A "logical pixel" might cover multiple screen pixels if scale > 1
    // Simple approach: draw a scaled rectangle.
    int scaledSize = round(state.scale);
    if (scaledSize < 1) scaledSize = 1;
    // Adjust sx, sy to be top-left of the block
    sx = round(sx - (scaledSize -1) / 2.0);
    sy = round(sy - (scaledSize -1) / 2.0);

    // Basic bounds check for the rectangle
    if (sx + scaledSize > 0 && sx < _canvas->width() && sy + scaledSize > 0 && sy < _canvas->height()) {
         _canvas->fillRect(sx, sy, scaledSize, scaledSize, state.color);
    }
}

void MicroPatternsDrawing::drawLine(float x1, float y1, float x2, float y2, const MicroPatternsState& state) {
    if (!_canvas) return;
    int sx1, sy1, sx2, sy2;
    transformPoint(x1, y1, state, sx1, sy1);
    transformPoint(x2, y2, state, sx2, sy2);
    rawLine(sx1, sy1, sx2, sy2, state.color);
}

void MicroPatternsDrawing::drawRect(float x, float y, float w, float h, const MicroPatternsState& state) {
    if (!_canvas) return;
    // Transform corners
    int sx1, sy1, sx2, sy2, sx3, sy3, sx4, sy4;
    transformPoint(x, y, state, sx1, sy1);             // Top-left
    transformPoint(x + w -1, y, state, sx2, sy2);         // Top-right
    transformPoint(x + w -1, y + h -1, state, sx3, sy3); // Bottom-right
    transformPoint(x, y + h -1, state, sx4, sy4);         // Bottom-left

    // Draw sides
    rawLine(sx1, sy1, sx2, sy2, state.color); // Top
    rawLine(sx2, sy2, sx3, sy3, state.color); // Right
    rawLine(sx3, sy3, sx4, sy4, state.color); // Bottom
    rawLine(sx4, sy4, sx1, sy1, state.color); // Left
}

// Simplified fillRect: Iterates through logical pixels, transforms center, draws scaled block with fill color.
// Does not handle complex polygon filling for rotated rectangles accurately.
void MicroPatternsDrawing::fillRect(float x, float y, float w, float h, const MicroPatternsState& state) {
    if (!_canvas) return;
    int scaledSize = round(state.scale);
     if (scaledSize < 1) scaledSize = 1;

    // Iterate through logical grid within the rectangle
    for (float ly = y; ly < y + h; ++ly) {
        for (float lx = x; lx < x + w; ++lx) {
            // Transform the center of this logical pixel
            int sx, sy;
            transformPoint(lx + 0.5f, ly + 0.5f, state, sx, sy);

            // Determine fill color based on the *screen* coordinate
            uint8_t fillColor = getFillColor(sx, sy, state);

            if (fillColor != COLOR_WHITE) {
                 // Adjust sx, sy to be top-left of the block for fillRect
                 int blockSX = round(sx - (scaledSize -1) / 2.0);
                 int blockSY = round(sy - (scaledSize -1) / 2.0);

                 // Basic bounds check
                 if (blockSX + scaledSize > 0 && blockSX < _canvas->width() && blockSY + scaledSize > 0 && blockSY < _canvas->height()) {
                    _canvas->fillRect(blockSX, blockSY, scaledSize, scaledSize, fillColor);
                 }
            }
        }
    }
}

// Simplified drawCircle: Transforms center, scales radius, uses midpoint algorithm on screen coords.
void MicroPatternsDrawing::drawCircle(float cx, float cy, float r, const MicroPatternsState& state) {
     if (!_canvas) return;
     int scx, scy;
     transformPoint(cx, cy, state, scx, scy);
     int scaledRadius = round(r * state.scale);
     if (scaledRadius < 1) scaledRadius = 1;

     // Basic Midpoint circle algorithm (operating on screen coordinates)
     int x = scaledRadius;
     int y = 0;
     int err = 1 - scaledRadius;

     while (x >= y) {
         // Draw 8 octants
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

// Simplified fillCircle: Iterates bounding box on screen, checks distance, uses fill color.
void MicroPatternsDrawing::fillCircle(float cx, float cy, float r, const MicroPatternsState& state) {
    if (!_canvas) return;
    int scx, scy;
    transformPoint(cx, cy, state, scx, scy);
    float scaledRadius = r * state.scale;
    if (scaledRadius < 0.5f) scaledRadius = 0.5f; // Minimum radius to draw something
    float rSquared = scaledRadius * scaledRadius;

    // Calculate screen bounding box
    int minX = floor(scx - scaledRadius);
    int minY = floor(scy - scaledRadius);
    int maxX = ceil(scx + scaledRadius);
    int maxY = ceil(scy + scaledRadius);

     // Clip bounding box to canvas
     minX = max(0, minX);
     minY = max(0, minY);
     maxX = min((int)_canvas->width(), maxX); // Cast canvas width to int
     maxY = min((int)_canvas->height(), maxY); // Cast canvas height to int
 
     // Iterate through screen pixels in the bounding box
    for (int sy = minY; sy < maxY; ++sy) {
        for (int sx = minX; sx < maxX; ++sx) {
            // Check if the center of the screen pixel is inside the circle
            float dx = (float)sx + 0.5f - scx;
            float dy = (float)sy + 0.5f - scy;
            if (dx * dx + dy * dy <= rSquared) {
                uint8_t fillColor = getFillColor(sx, sy, state);
                if (fillColor != COLOR_WHITE) {
                    rawPixel(sx, sy, fillColor);
                }
            }
        }
    }
}

// Draws a defined pattern asset
void MicroPatternsDrawing::drawAsset(float x, float y, const MicroPatternsAsset& asset, const MicroPatternsState& state) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0) return;

    int scaledSize = round(state.scale);
    if (scaledSize < 1) scaledSize = 1;

    // Iterate through the asset's logical pixels
    for (int iy = 0; iy < asset.height; ++iy) {
        for (int ix = 0; ix < asset.width; ++ix) {
            int index = iy * asset.width + ix;
            if (index < asset.data.size() && asset.data[index] == 1) { // If asset pixel is 'on' (black)
                // Calculate the logical position of this asset pixel relative to the asset's origin (x,y)
                float logicalPixelX = x + ix;
                float logicalPixelY = y + iy;

                // Transform this logical point's center to screen space
                int sx, sy;
                transformPoint(logicalPixelX + 0.5f, logicalPixelY + 0.5f, state, sx, sy);

                 // Adjust sx, sy to be top-left of the block for fillRect
                 int blockSX = round(sx - (scaledSize -1) / 2.0);
                 int blockSY = round(sy - (scaledSize -1) / 2.0);

                 // Draw the scaled block using the current state color (only if asset bit is 1)
                 // Basic bounds check
                 if (blockSX + scaledSize > 0 && blockSX < _canvas->width() && blockSY + scaledSize > 0 && blockSY < _canvas->height()) {
                    _canvas->fillRect(blockSX, blockSY, scaledSize, scaledSize, state.color);
                 }
            }
        }
    }
}

// Draws a pixel conditionally based on the fill pattern
void MicroPatternsDrawing::drawFilledPixel(float x, float y, const MicroPatternsState& state) {
    if (!_canvas) return;

    int scaledSize = round(state.scale);
    if (scaledSize < 1) scaledSize = 1;

    // Transform the center of the logical pixel
    int sx, sy;
    transformPoint(x + 0.5f, y + 0.5f, state, sx, sy);

    // Determine the effective color based on the fill asset at the screen coordinate
    uint8_t effectiveColor = getFillColor(sx, sy, state);

    // Draw the scaled block only if the effective color is not white
    if (effectiveColor != COLOR_WHITE) {
         // Adjust sx, sy to be top-left of the block for fillRect
         int blockSX = round(sx - (scaledSize -1) / 2.0);
         int blockSY = round(sy - (scaledSize -1) / 2.0);

         // Basic bounds check
         if (blockSX + scaledSize > 0 && blockSX < _canvas->width() && blockSY + scaledSize > 0 && blockSY < _canvas->height()) {
            _canvas->fillRect(blockSX, blockSY, scaledSize, scaledSize, effectiveColor);
         }
    }
}