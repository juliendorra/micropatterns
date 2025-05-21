#include "micropatterns_drawing.h"
#include "micropatterns_drawing.h"
#include <cmath> // For round, floor, ceil, sinf, cosf, fabs, sqrtf
#include <algorithm> // For std::min, std::max

MicroPatternsDrawing::MicroPatternsDrawing(M5EPD_Canvas* canvas)
    : _canvas(canvas), _interrupt_check_cb(nullptr), _usePixelOccupationMap(false), _overdrawSkippedPixels(0) {
    if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
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

void MicroPatternsDrawing::setInterruptCheckCallback(std::function<bool()> cb) {
    _interrupt_check_cb = cb;
}

void MicroPatternsDrawing::enablePixelOccupationMap(bool enable) {
    _usePixelOccupationMap = enable;
    if (_usePixelOccupationMap) {
        initPixelOccupationMap(); // Ensure it's sized
    }
}

void MicroPatternsDrawing::initPixelOccupationMap() {
    if (_canvasWidth > 0 && _canvasHeight > 0) {
        // Resize only if necessary or if size changed
        if (_pixelOccupationMap.size() != (size_t)_canvasWidth * _canvasHeight) {
            _pixelOccupationMap.resize((size_t)_canvasWidth * _canvasHeight, 0);
        }
        // No need to fill with 0 here, resetPixelOccupationMap will do it.
    } else {
        _pixelOccupationMap.clear();
    }
}

void MicroPatternsDrawing::resetPixelOccupationMap() {
    if (_usePixelOccupationMap && !_pixelOccupationMap.empty()) {
        std::fill(_pixelOccupationMap.begin(), _pixelOccupationMap.end(), 0);
    }
    _overdrawSkippedPixels = 0;
}

bool MicroPatternsDrawing::isPixelOccupied(int sx, int sy) const {
    if (!_usePixelOccupationMap || sx < 0 || sx >= _canvasWidth || sy < 0 || sy >= _canvasHeight) {
        return false; // Not using map or out of bounds
    }
    // Map should be initialized and sized correctly if _usePixelOccupationMap is true
    if (_pixelOccupationMap.empty()) return false;
    return _pixelOccupationMap[sy * _canvasWidth + sx] != 0;
}

void MicroPatternsDrawing::markPixelOccupied(int sx, int sy) {
    if (!_usePixelOccupationMap || sx < 0 || sx >= _canvasWidth || sy < 0 || sy >= _canvasHeight) {
        return; // Not using map or out of bounds
    }
    if (_pixelOccupationMap.empty()) return;
    _pixelOccupationMap[sy * _canvasWidth + sx] = 1; // Mark as occupied (1 is sufficient)
}

void MicroPatternsDrawing::clearCanvas() {
    if (_canvas) {
        _canvas->fillCanvas(DRAWING_COLOR_WHITE);
    }
    if (_usePixelOccupationMap) {
        resetPixelOccupationMap(); // Also reset occupation map when canvas is cleared
    }
}

// --- Transformation ---
// Uses DisplayListItem's snapshotted state
void MicroPatternsDrawing::transformPoint(float logical_x, float logical_y, const DisplayListItem& item, float& screen_x, float& screen_y) {
    float scaled_lx = logical_x * item.scaleFactor;
    float scaled_ly = logical_y * item.scaleFactor;
    matrix_apply_to_point(item.matrix, scaled_lx, scaled_ly, screen_x, screen_y);
}

void MicroPatternsDrawing::screenToLogicalBase(float screen_x, float screen_y, const DisplayListItem& item, float& base_logical_x, float& base_logical_y) {
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(item.inverseMatrix, screen_x, screen_y, scaled_logical_x, scaled_logical_y);

    if (item.scaleFactor == 0.0f) {
        base_logical_x = scaled_logical_x;
        base_logical_y = scaled_logical_y;
    } else {
        base_logical_x = scaled_logical_x / item.scaleFactor;
        base_logical_y = scaled_logical_y / item.scaleFactor;
    }
}


// --- Raw Drawing ---
void MicroPatternsDrawing::rawPixel(int sx, int sy, uint8_t color) {
    if (!_canvas) return;
    if (sx >= 0 && sx < _canvasWidth && sy >= 0 && sy < _canvasHeight) {
        if (_usePixelOccupationMap) {
            if (isPixelOccupied(sx, sy)) {
                _overdrawSkippedPixels++;
                return; // Pixel already occupied
            }
            markPixelOccupied(sx, sy);
        }
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
uint8_t MicroPatternsDrawing::getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const DisplayListItem& item) {
    if (!item.fillAsset) {
        return item.color; // Solid fill
    } else {
        const MicroPatternsAsset& asset = *item.fillAsset;
        if (asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return DRAWING_COLOR_WHITE; // Default to white if asset invalid

        float base_lx, base_ly;
        screenToLogicalBase(screen_pixel_center_x, screen_pixel_center_y, item, base_lx, base_ly);

        int assetX = static_cast<int>(floor(base_lx)) % asset.width;
        int assetY = static_cast<int>(floor(base_ly)) % asset.height;
        if (assetX < 0) assetX += asset.width;
        if (assetY < 0) assetY += asset.height;

        int index = assetY * asset.width + assetX;
        if (index >= 0 && index < (int)asset.data.size()) {
            uint8_t patternBit = asset.data[index]; // 0 or 1
            if (item.color == DRAWING_COLOR_WHITE) { // Inverted mode for FILL
                return patternBit == 1 ? DRAWING_COLOR_WHITE : DRAWING_COLOR_BLACK;
            } else { // Normal mode (item.color is DRAWING_COLOR_BLACK) for FILL
                return patternBit == 1 ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE;
            }
        }
        return item.color == DRAWING_COLOR_WHITE ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE; // Default on error
    }
}

// --- Drawing Primitives ---

void MicroPatternsDrawing::drawPixel(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

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

            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(item.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);

            float logical_pixel_start_x_scaled = static_cast<float>(lx) * item.scaleFactor;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * item.scaleFactor;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * item.scaleFactor;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * item.scaleFactor;
            
            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                rawPixel(sx_iter, sy_iter, item.color);
            }
        }
    }
}

void MicroPatternsDrawing::drawFilledPixel(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

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
            matrix_apply_to_point(item.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);
            
            float logical_pixel_start_x_scaled = static_cast<float>(lx) * item.scaleFactor;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * item.scaleFactor;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * item.scaleFactor;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * item.scaleFactor;

            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, item);
                rawPixel(sx_iter, sy_iter, fillColor);
            }
        }
    }
}


void MicroPatternsDrawing::drawLine(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx1 = item.intParams.at("X1");
    int ly1 = item.intParams.at("Y1");
    int lx2 = item.intParams.at("X2");
    int ly2 = item.intParams.at("Y2");

    float sx1_f, sy1_f, sx2_f, sy2_f;
    transformPoint(static_cast<float>(lx1), static_cast<float>(ly1), item, sx1_f, sy1_f);
    transformPoint(static_cast<float>(lx2), static_cast<float>(ly2), item, sx2_f, sy2_f);
    rawLine(static_cast<int>(round(sx1_f)), static_cast<int>(round(sy1_f)),
            static_cast<int>(round(sx2_f)), static_cast<int>(round(sy2_f)), item.color);
}

void MicroPatternsDrawing::drawRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");
    int lw = item.intParams.at("WIDTH");
    int lh = item.intParams.at("HEIGHT");
    if (lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), item, s_br_x, s_br_y);

    rawLine(round(s_tl_x), round(s_tl_y), round(s_tr_x), round(s_tr_y), item.color); // Top
    rawLine(round(s_tr_x), round(s_tr_y), round(s_br_x), round(s_br_y), item.color); // Right
    rawLine(round(s_br_x), round(s_br_y), round(s_bl_x), round(s_bl_y), item.color); // Bottom
    rawLine(round(s_bl_x), round(s_bl_y), round(s_tl_x), round(s_tl_y), item.color); // Left
}

void MicroPatternsDrawing::fillRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");
    int lw = item.intParams.at("WIDTH");
    int lh = item.intParams.at("HEIGHT");
    if (lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), item, s_br_x, s_br_y);

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
        if (_interrupt_check_cb && _interrupt_check_cb()) return; // Check interrupt
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            if (_interrupt_check_cb && _interrupt_check_cb()) return; // Check interrupt
    
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
            matrix_apply_to_point(item.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);

            float logical_rect_start_x_scaled = static_cast<float>(lx) * item.scaleFactor;
            float logical_rect_end_x_scaled = static_cast<float>(lx + lw) * item.scaleFactor;
            float logical_rect_start_y_scaled = static_cast<float>(ly) * item.scaleFactor;
            float logical_rect_end_y_scaled = static_cast<float>(ly + lh) * item.scaleFactor;

            if (scaled_logical_x >= logical_rect_start_x_scaled && scaled_logical_x < logical_rect_end_x_scaled &&
                scaled_logical_y >= logical_rect_start_y_scaled && scaled_logical_y < logical_rect_end_y_scaled) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, item);
                // No check for DRAWING_COLOR_WHITE, draw all pixels for fill.
                rawPixel(sx_iter, sy_iter, fillColor);
            }
        }
    }
    esp_task_wdt_reset(); // Ensure WDT is reset after the loop
}

void MicroPatternsDrawing::drawCircle(const DisplayListItem& item) {
    if (!_canvas) return;
    int lcx = item.intParams.at("X");
    int lcy = item.intParams.at("Y");
    int lr = item.intParams.at("RADIUS");
    if (lr <= 0) return;
     
    float scx_f, scy_f;
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy), item, scx_f, scy_f);
     
    float mat_scale_x = sqrtf(item.matrix[0]*item.matrix[0] + item.matrix[1]*item.matrix[1]);
    float mat_scale_y = sqrtf(item.matrix[2]*item.matrix[2] + item.matrix[3]*item.matrix[3]);
    float screen_radius_approx = static_cast<float>(lr) * item.scaleFactor * std::max(mat_scale_x, mat_scale_y);
     
    int scx = static_cast<int>(round(scx_f));
    int scy = static_cast<int>(round(scy_f));
    int scaledRadius = static_cast<int>(round(screen_radius_approx));
    if (scaledRadius < 1) scaledRadius = 1;

    int x_coord = scaledRadius;
    int y_coord = 0;
    int err = 1 - scaledRadius;

    while (x_coord >= y_coord) {
        rawPixel(scx + x_coord, scy + y_coord, item.color); rawPixel(scx + y_coord, scy + x_coord, item.color);
        rawPixel(scx - y_coord, scy + x_coord, item.color); rawPixel(scx - x_coord, scy + y_coord, item.color);
        rawPixel(scx - x_coord, scy - y_coord, item.color); rawPixel(scx - y_coord, scy - x_coord, item.color);
        rawPixel(scx + y_coord, scy - x_coord, item.color); rawPixel(scx + x_coord, scy - y_coord, item.color);
        y_coord++;
        if (err <= 0) {
            err += 2 * y_coord + 1;
        } else {
            x_coord--;
            err += 2 * (y_coord - x_coord) + 1;
        }
    }
}

void MicroPatternsDrawing::fillCircle(const DisplayListItem& item) {
    if (!_canvas) return;
    int lcx = item.intParams.at("X");
    int lcy = item.intParams.at("Y");
    int lr = item.intParams.at("RADIUS");
    if (lr <= 0) return;

    float logical_radius = static_cast<float>(lr);
    float s_pts_x[8], s_pts_y[8];
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy - logical_radius), item, s_pts_x[0], s_pts_y[0]);
    transformPoint(static_cast<float>(lcx + logical_radius), static_cast<float>(lcy), item, s_pts_x[1], s_pts_y[1]);
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy + logical_radius), item, s_pts_x[2], s_pts_y[2]);
    transformPoint(static_cast<float>(lcx - logical_radius), static_cast<float>(lcy), item, s_pts_x[3], s_pts_y[3]);
    float diag_offset = logical_radius * 0.7071f;
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy - diag_offset), item, s_pts_x[4], s_pts_y[4]);
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy + diag_offset), item, s_pts_x[5], s_pts_y[5]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy + diag_offset), item, s_pts_x[6], s_pts_y[6]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy - diag_offset), item, s_pts_x[7], s_pts_y[7]);

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
        if (_interrupt_check_cb && _interrupt_check_cb()) return; // Check interrupt
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            if (_interrupt_check_cb && _interrupt_check_cb()) return; // Check interrupt
    
            if (pixelCount > 0 && pixelCount % 2000 == 0) {
                yield();
                if (pixelCount % 8000 == 0) {
                    esp_task_wdt_reset();
                }
            }
            pixelCount++;
    
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float base_logical_x, base_logical_y;
            screenToLogicalBase(screen_center_x, screen_center_y, item, base_logical_x, base_logical_y);

            float dx = base_logical_x - lcx;
            float dy = base_logical_y - lcy;

            if (dx * dx + dy * dy <= logical_radius_sq) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, item);
                rawPixel(sx_iter, sy_iter, fillColor);
            }
        }
    }
    esp_task_wdt_reset();
}

void MicroPatternsDrawing::drawAsset(const DisplayListItem& item, const MicroPatternsAsset& asset) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return;
    int lx_asset_origin = item.intParams.at("X");
    int ly_asset_origin = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin + asset.height), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin + asset.height), item, s_br_x, s_br_y);

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
        if (_interrupt_check_cb && _interrupt_check_cb()) return;
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            if (_interrupt_check_cb && _interrupt_check_cb()) return;
            
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
            screenToLogicalBase(screen_center_x, screen_center_y, item, base_logical_x, base_logical_y);

            float asset_local_x = base_logical_x - lx_asset_origin;
            float asset_local_y = base_logical_y - ly_asset_origin;

            if (asset_local_x >= 0 && asset_local_x < asset.width &&
                asset_local_y >= 0 && asset_local_y < asset.height) {
                
                int asset_ix = static_cast<int>(floor(asset_local_x));
                int asset_iy = static_cast<int>(floor(asset_local_y));
                int asset_data_index = asset_iy * asset.width + asset_ix;

                if (asset_data_index >= 0 && asset_data_index < (int)asset.data.size() && asset.data[asset_data_index] == 1) {
                    rawPixel(sx_iter, sy_iter, item.color); // DRAW uses item.color
                }
            }
        }
    }
    esp_task_wdt_reset();
}