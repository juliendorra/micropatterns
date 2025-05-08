#include "micropatterns_drawing.h"
#include <cmath> // For round, floor, ceil, sinf, cosf, fabs, sqrtf
#include <Arduino.h> // For yield()
#include <algorithm> // For std::min, std::max

MicroPatternsDrawing::MicroPatternsDrawing(M5EPD_Canvas* canvas) : _canvas(canvas) {
    if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
    // precomputeTrigTables(); // Removed, matrix_make_rotation uses sinf/cosf directly
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
        _canvas->fillCanvas(DRAWING_COLOR_WHITE);
    }
}

// --- Transformation ---
void MicroPatternsDrawing::transformPoint(float logical_x, float logical_y, const MicroPatternsState& state, float& screen_x, float& screen_y) {
    // 1. Apply absolute scale factor first
    float scaled_lx = logical_x * state.scale;
    float scaled_ly = logical_y * state.scale;

    // 2. Apply the cumulative transformation matrix
    matrix_apply_to_point(state.matrix, scaled_lx, scaled_ly, screen_x, screen_y);
}

void MicroPatternsDrawing::screenToLogicalBase(float screen_x, float screen_y, const MicroPatternsState& state, float& base_logical_x, float& base_logical_y) {
    // 1. Apply inverse of the cumulative transformation matrix
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(state.inverseMatrix, screen_x, screen_y, scaled_logical_x, scaled_logical_y);

    // 2. Undo the absolute scale factor
    if (state.scale == 0.0f) { // Avoid division by zero
        base_logical_x = scaled_logical_x; // Or handle as an error/default
        base_logical_y = scaled_logical_y;
    } else {
        base_logical_x = scaled_logical_x / state.scale;
        base_logical_y = scaled_logical_y / state.scale;
    }
}


// --- Raw Drawing ---
void MicroPatternsDrawing::rawPixel(int sx, int sy, uint8_t color) {
    if (!_canvas) return;
    if (sx >= 0 && sx < _canvasWidth && sy >= 0 && sy < _canvasHeight) {
        _canvas->drawPixel(sx, sy, color);
    }
}

void MicroPatternsDrawing::rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color) {
    if (!_canvas) return;

    int dx_abs = abs(sx2 - sx1);
    int dy_abs = -abs(sy2 - sy1); // dy is negative for typical algorithm
    int current_sx = sx1;
    int current_sy = sy1;
    int stepX = (sx1 < sx2) ? 1 : -1;
    int stepY = (sy1 < sy2) ? 1 : -1;
    int err = dx_abs + dy_abs; // Error term

    while (true) {
        rawPixel(current_sx, current_sy, color);
        if (current_sx == sx2 && current_sy == sy2) break;
        int e2 = 2 * err;
        if (e2 >= dy_abs) { // Favor X step
            if (current_sx == sx2) break; // Reached end in X
            err += dy_abs;
            current_sx += stepX;
        }
        if (e2 <= dx_abs) { // Favor Y step
            if (current_sy == sy2) break; // Reached end in Y
            err += dx_abs;
            current_sy += stepY;
        }
    }
}

// --- Fill Pattern Helper ---
uint8_t MicroPatternsDrawing::getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const MicroPatternsState& state) {
    if (!state.fillAsset) {
        return state.color; // Solid fill
    } else {
        const MicroPatternsAsset& asset = *state.fillAsset;
        if (asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return DRAWING_COLOR_WHITE;

        float base_lx, base_ly;
        screenToLogicalBase(screen_pixel_center_x, screen_pixel_center_y, state, base_lx, base_ly);

        // Tiling logic using base logical coordinates
        int assetX = static_cast<int>(floor(base_lx)) % asset.width;
        int assetY = static_cast<int>(floor(base_ly)) % asset.height;
        if (assetX < 0) assetX += asset.width;
        if (assetY < 0) assetY += asset.height;

        int index = assetY * asset.width + assetX;
        if (index >= 0 && index < asset.data.size()) {
            return asset.data[index] == 1 ? state.color : DRAWING_COLOR_WHITE;
        }
        // log_e("Fill asset index out of bounds: base_lx=%.2f, base_ly=%.2f -> asset (%d, %d) -> index %d. Screen (%.2f, %.2f)", base_lx, base_ly, assetX, assetY, index, screen_pixel_center_x, screen_pixel_center_y);
        return DRAWING_COLOR_WHITE; // Default to white on error
    }
}

// --- Drawing Primitives ---

void MicroPatternsDrawing::drawPixel(int lx, int ly, const MicroPatternsState& state) {
    if (!_canvas) return;

    // A logical pixel (lx,ly) covers the area [lx, lx+1) x [ly, ly+1) in logical space.
    // Transform its 4 corners to screen space.
    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), state, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), state, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), state, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), state, s_br_x, s_br_y);

    // Determine screen-space bounding box (rounded to int for iteration)
    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    // Clip to canvas
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            // Transform screen pixel center back to scaled logical coordinates
            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(state.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);

            // Check if this scaled logical point is within the original logical pixel's scaled boundaries
            float logical_pixel_start_x_scaled = static_cast<float>(lx) * state.scale;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * state.scale;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * state.scale;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * state.scale;
            
            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                rawPixel(sx_iter, sy_iter, state.color);
            }
        }
    }
}

void MicroPatternsDrawing::drawFilledPixel(int lx, int ly, const MicroPatternsState& state) {
    if (!_canvas) return;
    // Similar to drawPixel, but uses getFillColor
    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), state, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), state, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), state, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), state, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(state.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);
            
            float logical_pixel_start_x_scaled = static_cast<float>(lx) * state.scale;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * state.scale;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * state.scale;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * state.scale;

            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, state);
                if (fillColor != DRAWING_COLOR_WHITE) {
                    rawPixel(sx_iter, sy_iter, fillColor);
                }
            }
        }
    }
}


void MicroPatternsDrawing::drawLine(int lx1, int ly1, int lx2, int ly2, const MicroPatternsState& state) {
    if (!_canvas) return;
    float sx1_f, sy1_f, sx2_f, sy2_f;
    transformPoint(static_cast<float>(lx1), static_cast<float>(ly1), state, sx1_f, sy1_f);
    transformPoint(static_cast<float>(lx2), static_cast<float>(ly2), state, sx2_f, sy2_f);
    rawLine(static_cast<int>(round(sx1_f)), static_cast<int>(round(sy1_f)),
            static_cast<int>(round(sx2_f)), static_cast<int>(round(sy2_f)), state.color);
}

void MicroPatternsDrawing::drawRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state) {
    if (!_canvas || lw <= 0 || lh <= 0) return;
    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), state, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), state, s_tr_x, s_tr_y); // Use lw, not lw-1 for rect "edge"
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), state, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), state, s_br_x, s_br_y);

    rawLine(round(s_tl_x), round(s_tl_y), round(s_tr_x), round(s_tr_y), state.color); // Top
    rawLine(round(s_tr_x), round(s_tr_y), round(s_br_x), round(s_br_y), state.color); // Right
    rawLine(round(s_br_x), round(s_br_y), round(s_bl_x), round(s_bl_y), state.color); // Bottom
    rawLine(round(s_bl_x), round(s_bl_y), round(s_tl_x), round(s_tl_y), state.color); // Left
}

void MicroPatternsDrawing::fillRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state) {
    if (!_canvas || lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), state, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), state, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), state, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), state, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    int pixelCount = 0;
    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            if (pixelCount > 0 && pixelCount % 2000 == 0) { // Yield less frequently for fillRect
                yield();
                if (pixelCount % 8000 == 0) {
                    esp_task_wdt_reset();
                }
            }
            pixelCount++;

            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(state.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);

            float logical_rect_start_x_scaled = static_cast<float>(lx) * state.scale;
            float logical_rect_end_x_scaled = static_cast<float>(lx + lw) * state.scale;
            float logical_rect_start_y_scaled = static_cast<float>(ly) * state.scale;
            float logical_rect_end_y_scaled = static_cast<float>(ly + lh) * state.scale;

            if (scaled_logical_x >= logical_rect_start_x_scaled && scaled_logical_x < logical_rect_end_x_scaled &&
                scaled_logical_y >= logical_rect_start_y_scaled && scaled_logical_y < logical_rect_end_y_scaled) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, state);
                if (fillColor != DRAWING_COLOR_WHITE) {
                    rawPixel(sx_iter, sy_iter, fillColor);
                }
            }
        }
    }
    esp_task_wdt_reset(); // Ensure WDT is reset after the loop
}

void MicroPatternsDrawing::drawCircle(int lcx, int lcy, int lr, const MicroPatternsState& state) {
     if (!_canvas || lr <= 0) return;
     
     // For outline, we can approximate by transforming many points on the circle's circumference
     // or by transforming the center and scaling the radius (less accurate for non-uniform scale/shear).
     // A simpler approach for now: transform center, scale radius, draw screen-space circle.
     float scx_f, scy_f;
     transformPoint(static_cast<float>(lcx), static_cast<float>(lcy), state, scx_f, scy_f);
     
     // Estimate screen radius. This is an approximation.
     // Consider the maximum scaling effect of the matrix part.
     float mat_scale_x = sqrtf(state.matrix[0]*state.matrix[0] + state.matrix[1]*state.matrix[1]);
     float mat_scale_y = sqrtf(state.matrix[2]*state.matrix[2] + state.matrix[3]*state.matrix[3]);
     float screen_radius_approx = static_cast<float>(lr) * state.scale * std::max(mat_scale_x, mat_scale_y);
     
     int scx = static_cast<int>(round(scx_f));
     int scy = static_cast<int>(round(scy_f));
     int scaledRadius = static_cast<int>(round(screen_radius_approx));
     if (scaledRadius < 1) scaledRadius = 1;

     // Basic Midpoint circle algorithm
     int x = scaledRadius;
     int y = 0;
     int err = 1 - scaledRadius;

     while (x >= y) {
         rawPixel(scx + x, scy + y, state.color); rawPixel(scx + y, scy + x, state.color);
         rawPixel(scx - y, scy + x, state.color); rawPixel(scx - x, scy + y, state.color);
         rawPixel(scx - x, scy - y, state.color); rawPixel(scx - y, scy - x, state.color);
         rawPixel(scx + y, scy - x, state.color); rawPixel(scx + x, scy - y, state.color);
         y++;
         if (err <= 0) {
             err += 2 * y + 1;
         } else {
             x--;
             err += 2 * (y - x) + 1;
         }
     }
}

void MicroPatternsDrawing::fillCircle(int lcx, int lcy, int lr, const MicroPatternsState& state) {
    if (!_canvas || lr <= 0) return;

    // Transform the 8 points on the bounding box of the logical circle to find screen bounding box
    // This is more robust than just transforming center and scaling radius for arbitrary transforms.
    float logical_radius = static_cast<float>(lr);
    float s_pts_x[8], s_pts_y[8];
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy - logical_radius), state, s_pts_x[0], s_pts_y[0]); // Top
    transformPoint(static_cast<float>(lcx + logical_radius), static_cast<float>(lcy), state, s_pts_x[1], s_pts_y[1]); // Right
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy + logical_radius), state, s_pts_x[2], s_pts_y[2]); // Bottom
    transformPoint(static_cast<float>(lcx - logical_radius), static_cast<float>(lcy), state, s_pts_x[3], s_pts_y[3]); // Left
    // Diagonal points for better bounding box with rotation
    float diag_offset = logical_radius * 0.7071f; // approx 1/sqrt(2)
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy - diag_offset), state, s_pts_x[4], s_pts_y[4]);
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy + diag_offset), state, s_pts_x[5], s_pts_y[5]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy + diag_offset), state, s_pts_x[6], s_pts_y[6]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy - diag_offset), state, s_pts_x[7], s_pts_y[7]);

    float min_sx_f = s_pts_x[0], max_sx_f = s_pts_x[0];
    float min_sy_f = s_pts_y[0], max_sy_f = s_pts_y[0];
    for(int i=1; i<8; ++i) {
        min_sx_f = std::min(min_sx_f, s_pts_x[i]); max_sx_f = std::max(max_sx_f, s_pts_x[i]);
        min_sy_f = std::min(min_sy_f, s_pts_y[i]); max_sy_f = std::max(max_sy_f, s_pts_y[i]);
    }

    int min_sx = static_cast<int>(floor(min_sx_f));
    int max_sx = static_cast<int>(ceil(max_sx_f));
    int min_sy = static_cast<int>(floor(min_sy_f));
    int max_sy = static_cast<int>(ceil(max_sy_f));
    
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    float logical_radius_sq = logical_radius * logical_radius;
    int pixelCount = 0;

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            if (pixelCount > 0 && pixelCount % 2000 == 0) {
                yield();
                if (pixelCount % 8000 == 0) {
                    esp_task_wdt_reset();
                }
            }
            pixelCount++;

            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float base_logical_x, base_logical_y; // Use base logical for distance check
            screenToLogicalBase(screen_center_x, screen_center_y, state, base_logical_x, base_logical_y);

            float dx = base_logical_x - lcx;
            float dy = base_logical_y - lcy;

            if (dx * dx + dy * dy <= logical_radius_sq) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, state);
                if (fillColor != DRAWING_COLOR_WHITE) {
                    rawPixel(sx_iter, sy_iter, fillColor);
                }
            }
        }
    }
    esp_task_wdt_reset();
}

void MicroPatternsDrawing::drawAsset(int lx_asset_origin, int ly_asset_origin, const MicroPatternsAsset& asset, const MicroPatternsState& state) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin), state, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin), state, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin + asset.height), state, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin + asset.height), state, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    int pixelCount = 0;
    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
             if (pixelCount > 0 && pixelCount % 1000 == 0) {
                yield();
                 if (pixelCount % 4000 == 0) {
                    esp_task_wdt_reset();
                }
            }
            pixelCount++;

            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float base_logical_x, base_logical_y;
            screenToLogicalBase(screen_center_x, screen_center_y, state, base_logical_x, base_logical_y);

            // Convert base_logical_x/y to asset's local coordinates
            float asset_local_x = base_logical_x - lx_asset_origin;
            float asset_local_y = base_logical_y - ly_asset_origin;

            if (asset_local_x >= 0 && asset_local_x < asset.width &&
                asset_local_y >= 0 && asset_local_y < asset.height) {
                
                int asset_ix = static_cast<int>(floor(asset_local_x));
                int asset_iy = static_cast<int>(floor(asset_local_y));
                int asset_data_index = asset_iy * asset.width + asset_ix;

                if (asset_data_index >= 0 && asset_data_index < asset.data.size() && asset.data[asset_data_index] == 1) {
                    rawPixel(sx_iter, sy_iter, state.color); // DRAW uses current state.color
                }
            }
        }
    }
    esp_task_wdt_reset();
}