#include "micropatterns_drawing.h"
#include "IDrawingHAL.h"
#include "matrix_utils.h" // For matrix_apply_to_point
#include "esp32-hal-log.h" // For logging
#include <cmath>          // For roundf, floorf, ceilf, std::min, std::max, M_PI, sinf, cosf
#include <algorithm>      // For std::min, std::max

// Constructor
MicroPatternsDrawing::MicroPatternsDrawing(IDrawingHAL* hal) : _hal(hal), _interrupt_check_cb(nullptr) {}

void MicroPatternsDrawing::setInterruptCheckCallback(std::function<bool()> cb) {
    _interrupt_check_cb = cb;
}

void MicroPatternsDrawing::clearCanvas() {
    if (!_hal) return;
    _hal->clearScreen();
}

// --- Transformation Helpers (public as per header) ---
void MicroPatternsDrawing::transformPoint(float logical_x, float logical_y, const float matrix[6], float scaleFactor, float& screen_x, float& screen_y) {
    float scaled_lx = logical_x * scaleFactor;
    float scaled_ly = logical_y * scaleFactor;
    matrix_apply_to_point(matrix, scaled_lx, scaled_ly, screen_x, screen_y);
}

void MicroPatternsDrawing::screenToLogicalBase(float screen_x, float screen_y, const float inverseMatrix[6], float scaleFactor, float& base_logical_x, float& base_logical_y) {
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(inverseMatrix, screen_x, screen_y, scaled_logical_x, scaled_logical_y);

    if (scaleFactor == 0.0f) {
        base_logical_x = scaled_logical_x;
        base_logical_y = scaled_logical_y;
    } else {
        base_logical_x = scaled_logical_x / scaleFactor;
        base_logical_y = scaled_logical_y / scaleFactor;
    }
}

// --- Raw Drawing Methods ---
void MicroPatternsDrawing::_rawPixel(int16_t sx, int16_t sy, uint16_t color) {
    if (!_hal) return;
    _hal->drawPixel(sx, sy, color);
}

void MicroPatternsDrawing::_rawLine(int16_t sx1, int16_t sy1, int16_t sx2, int16_t sy2, uint16_t color) {
    if (!_hal) return;
    _hal->drawLine(sx1, sy1, sx2, sy2, color);
}

// --- Fill Pattern Helper ---
uint16_t MicroPatternsDrawing::_getFillColor(float logical_x, float logical_y, const DisplayListItem& item) {
    if (!item.fillAsset) {
        return item.color; // Solid fill
    } else {
        const MicroPatternsAsset& asset = *item.fillAsset;
        if (asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return HAL_COLOR_WHITE;

        int assetX = static_cast<int>(floorf(logical_x)) % asset.width;
        int assetY = static_cast<int>(floorf(logical_y)) % asset.height;
        if (assetX < 0) assetX += asset.width;
        if (assetY < 0) assetY += asset.height;

        int index = assetY * asset.width + assetX;
        if (index >= 0 && index < (int)asset.data.size()) {
            uint8_t patternBit = asset.data[index];
            if (item.color == HAL_COLOR_WHITE) {
                return patternBit == 1 ? HAL_COLOR_WHITE : HAL_COLOR_BLACK;
            } else {
                return patternBit == 1 ? HAL_COLOR_BLACK : HAL_COLOR_WHITE;
            }
        }
        return (item.color == HAL_COLOR_WHITE) ? HAL_COLOR_BLACK : HAL_COLOR_WHITE;
    }
}

// --- Drawing Primitives using DisplayListItem ---

void MicroPatternsDrawing::drawPixel(const DisplayListItem& item) {
    if (!_hal) return;
    float lx = static_cast<float>(item.intParams.at("X"));
    float ly = static_cast<float>(item.intParams.at("Y"));
    float sx, sy;
    transformPoint(lx + 0.5f, ly + 0.5f, item.matrix, item.scaleFactor, sx, sy);
    _rawPixel(static_cast<int16_t>(roundf(sx)), static_cast<int16_t>(roundf(sy)), item.color);
}

void MicroPatternsDrawing::fillRect(const DisplayListItem& item) {
    if (!_hal) return;
    float lx = static_cast<float>(item.intParams.at("X"));
    float ly = static_cast<float>(item.intParams.at("Y"));
    float lw = static_cast<float>(item.intParams.at("WIDTH"));
    float lh = static_cast<float>(item.intParams.at("HEIGHT"));
    if (lw <= 0 || lh <= 0) return;

    // Simplified approach: Transform 4 corners, then rasterize the bounding box.
    // This is a more general approach than assuming HAL can handle transformed rects.
    float s_pts_x[4], s_pts_y[4];
    transformPoint(lx, ly, item.matrix, item.scaleFactor, s_pts_x[0], s_pts_y[0]);
    transformPoint(lx + lw, ly, item.matrix, item.scaleFactor, s_pts_x[1], s_pts_y[1]);
    transformPoint(lx + lw, ly + lh, item.matrix, item.scaleFactor, s_pts_x[2], s_pts_y[2]);
    transformPoint(lx, ly + lh, item.matrix, item.scaleFactor, s_pts_x[3], s_pts_y[3]);

    float min_sx = floorf(std::min({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]}));
    float max_sx = ceilf(std::max({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]}));
    float min_sy = floorf(std::min({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]}));
    float max_sy = ceilf(std::max({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]}));
    
    min_sx = std::max(0.0f, min_sx);
    min_sy = std::max(0.0f, min_sy);
    max_sx = std::min((float)_hal->getScreenWidth(), max_sx);
    max_sy = std::min((float)_hal->getScreenHeight(), max_sy);

    for (int16_t sy_iter = static_cast<int16_t>(min_sy); sy_iter < static_cast<int16_t>(max_sy); ++sy_iter) {
        if (_interrupt_check_cb && _interrupt_check_cb()) return;
        for (int16_t sx_iter = static_cast<int16_t>(min_sx); sx_iter < static_cast<int16_t>(max_sx); ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;
            float logical_x_test, logical_y_test;
            screenToLogicalBase(screen_center_x, screen_center_y, item.inverseMatrix, item.scaleFactor, logical_x_test, logical_y_test);
            
            if (logical_x_test >= lx && logical_x_test < (lx + lw) &&
                logical_y_test >= ly && logical_y_test < (ly + lh)) {
                uint16_t pixelColor = _getFillColor(logical_x_test, logical_y_test, item);
                _rawPixel(sx_iter, sy_iter, pixelColor);
            }
        }
    }
}

void MicroPatternsDrawing::drawLine(const DisplayListItem& item) {
    if (!_hal) return;
    float lx1 = static_cast<float>(item.intParams.at("X1"));
    float ly1 = static_cast<float>(item.intParams.at("Y1"));
    float lx2 = static_cast<float>(item.intParams.at("X2"));
    float ly2 = static_cast<float>(item.intParams.at("Y2"));
    float sx1_f, sy1_f, sx2_f, sy2_f;
    transformPoint(lx1, ly1, item.matrix, item.scaleFactor, sx1_f, sy1_f);
    transformPoint(lx2, ly2, item.matrix, item.scaleFactor, sx2_f, sy2_f);
    _rawLine(static_cast<int16_t>(roundf(sx1_f)), static_cast<int16_t>(roundf(sy1_f)),
             static_cast<int16_t>(roundf(sx2_f)), static_cast<int16_t>(roundf(sy2_f)), item.color);
}

void MicroPatternsDrawing::drawRect(const DisplayListItem& item) {
    if (!_hal) return;
    float lx = static_cast<float>(item.intParams.at("X"));
    float ly = static_cast<float>(item.intParams.at("Y"));
    float lw = static_cast<float>(item.intParams.at("WIDTH"));
    float lh = static_cast<float>(item.intParams.at("HEIGHT"));
    if (lw <= 0 || lh <= 0) return;

    float s_pts_x[4], s_pts_y[4];
    transformPoint(lx, ly, item.matrix, item.scaleFactor, s_pts_x[0], s_pts_y[0]);
    transformPoint(lx + lw, ly, item.matrix, item.scaleFactor, s_pts_x[1], s_pts_y[1]);
    transformPoint(lx + lw, ly + lh, item.matrix, item.scaleFactor, s_pts_x[2], s_pts_y[2]);
    transformPoint(lx, ly + lh, item.matrix, item.scaleFactor, s_pts_x[3], s_pts_y[3]);

    _rawLine(roundf(s_pts_x[0]), roundf(s_pts_y[0]), roundf(s_pts_x[1]), roundf(s_pts_y[1]), item.color);
    _rawLine(roundf(s_pts_x[1]), roundf(s_pts_y[1]), roundf(s_pts_x[2]), roundf(s_pts_y[2]), item.color);
    _rawLine(roundf(s_pts_x[2]), roundf(s_pts_y[2]), roundf(s_pts_x[3]), roundf(s_pts_y[3]), item.color);
    _rawLine(roundf(s_pts_x[3]), roundf(s_pts_y[3]), roundf(s_pts_x[0]), roundf(s_pts_y[0]), item.color);
}

void MicroPatternsDrawing::drawCircle(const DisplayListItem& item) {
    if (!_hal) return;
    float lcx = static_cast<float>(item.intParams.at("X"));
    float lcy = static_cast<float>(item.intParams.at("Y"));
    float lr = static_cast<float>(item.intParams.at("RADIUS"));
    if (lr <= 0) return;

    // Approximate by drawing a polygon (e.g., 20-sided)
    const int segments = 20;
    float sx_prev, sy_prev;
    transformPoint(lcx + lr, lcy, item.matrix, item.scaleFactor, sx_prev, sy_prev); // Point on circle edge

    for (int i = 1; i <= segments; ++i) {
        float angle = 2 * M_PI * i / segments;
        float lx_curr = lcx + lr * cosf(angle);
        float ly_curr = lcy + lr * sinf(angle);
        float sx_curr, sy_curr;
        transformPoint(lx_curr, ly_curr, item.matrix, item.scaleFactor, sx_curr, sy_curr);
        _rawLine(roundf(sx_prev), roundf(sy_prev), roundf(sx_curr), roundf(sy_curr), item.color);
        sx_prev = sx_curr;
        sy_prev = sy_curr;
    }
}

void MicroPatternsDrawing::fillCircle(const DisplayListItem& item) {
    if (!_hal) return;
    float lcx = static_cast<float>(item.intParams.at("X"));
    float lcy = static_cast<float>(item.intParams.at("Y"));
    float lr = static_cast<float>(item.intParams.at("RADIUS"));
    if (lr <= 0) return;

    // Estimate screen bounding box (same as old fillCircle)
    float s_center_x, s_center_y;
    transformPoint(lcx, lcy, item.matrix, item.scaleFactor, s_center_x, s_center_y);
    float mat_scale_x = sqrtf(item.matrix[0]*item.matrix[0] + item.matrix[1]*item.matrix[1]);
    float mat_scale_y = sqrtf(item.matrix[2]*item.matrix[2] + item.matrix[3]*item.matrix[3]);
    float effective_radius_on_screen = lr * item.scaleFactor * std::max(mat_scale_x, mat_scale_y);

    float min_sx = floorf(s_center_x - effective_radius_on_screen);
    float max_sx = ceilf(s_center_x + effective_radius_on_screen);
    float min_sy = floorf(s_center_y - effective_radius_on_screen);
    float max_sy = ceilf(s_center_y + effective_radius_on_screen);

    min_sx = std::max(0.0f, min_sx);
    min_sy = std::max(0.0f, min_sy);
    max_sx = std::min((float)_hal->getScreenWidth(), max_sx);
    max_sy = std::min((float)_hal->getScreenHeight(), max_sy);

    float logical_radius_sq = lr * lr;
    for (int16_t sy_iter = static_cast<int16_t>(min_sy); sy_iter < static_cast<int16_t>(max_sy); ++sy_iter) {
        if (_interrupt_check_cb && _interrupt_check_cb()) return;
        for (int16_t sx_iter = static_cast<int16_t>(min_sx); sx_iter < static_cast<int16_t>(max_sx); ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;
            float logical_x_test, logical_y_test;
            screenToLogicalBase(screen_center_x, screen_center_y, item.inverseMatrix, item.scaleFactor, logical_x_test, logical_y_test);
            float dx = logical_x_test - lcx;
            float dy = logical_y_test - lcy;
            if (dx * dx + dy * dy <= logical_radius_sq) {
                uint16_t pixelColor = _getFillColor(logical_x_test, logical_y_test, item);
                _rawPixel(sx_iter, sy_iter, pixelColor);
            }
        }
    }
}

void MicroPatternsDrawing::drawText(const DisplayListItem& item) {
    if (!_hal) return;
    String text_content = item.stringParams.count("TEXT") ? item.stringParams.at("TEXT") : "";
    if (text_content.isEmpty()) return;

    float lx = static_cast<float>(item.intParams.at("X"));
    float ly = static_cast<float>(item.intParams.at("Y"));
    uint8_t textSize = static_cast<uint8_t>(item.intParams.count("SIZE") ? item.intParams.at("SIZE") : 1);
    float sx, sy;
    transformPoint(lx, ly, item.matrix, item.scaleFactor, sx, sy);

    _hal->setTextColor(item.color);
    _hal->setTextSize(textSize);
    _hal->setCursor(static_cast<int16_t>(roundf(sx)), static_cast<int16_t>(roundf(sy)));
    _hal->print(text_content.c_str());
}

void MicroPatternsDrawing::drawBitmap(const DisplayListItem& item, const MicroPatternsAsset* asset) {
    if (!_hal || !asset || asset->data.empty()) return;
    float lx = static_cast<float>(item.intParams.at("X"));
    float ly = static_cast<float>(item.intParams.at("Y"));
    int16_t w = static_cast<int16_t>(asset->width);
    int16_t h = static_cast<int16_t>(asset->height);
    float sx, sy;
    transformPoint(lx, ly, item.matrix, item.scaleFactor, sx, sy); // Transform top-left

    // NOTE: This simple transformation of the top-left corner is insufficient for scaled/rotated bitmaps.
    // True transformed bitmap drawing requires iterating through screen pixels in the transformed bounding box
    // and sampling from the source bitmap using inverse transformation, similar to fillRect/fillCircle.
    // The current IDrawingHAL::drawBitmap likely expects axis-aligned screen coordinates.
    // For now, this will draw the bitmap axis-aligned at the transformed top-left sx, sy.
    _hal->drawBitmap(static_cast<int16_t>(roundf(sx)), static_cast<int16_t>(roundf(sy)), 
                     asset->data.data(), w, h, item.color);
}
