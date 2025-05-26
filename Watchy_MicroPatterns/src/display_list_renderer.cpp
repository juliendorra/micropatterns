#include "display_list_renderer.h"
#include "esp32-hal-log.h"
#include <algorithm> // For std::min, std::max
#include <cmath>     // For sqrtf, floor, ceil, round, std::hypot

// Constructor now takes MicroPatternsDrawing reference
DisplayListRenderer::DisplayListRenderer(MicroPatternsDrawing& drawer,
                                       const std::map<String, MicroPatternsAsset>& assets,
                                       int canvasWidth, int canvasHeight)
    : _drawer(drawer), // Initialize _drawer reference
      _assets(assets),
      _occlusionBuffer(canvasWidth, canvasHeight, 16), 
      _canvasWidth(canvasWidth),
      _canvasHeight(canvasHeight),
      _totalItems(0), _renderedItems(0), _culledOffScreen(0), _culledByOcclusion(0),
      _interrupt_check_cb(nullptr) {
    // _drawer.setCanvas is not needed if _drawer is constructed with its HAL already.
}

void DisplayListRenderer::setInterruptCheckCallback(std::function<bool()> cb) {
    _interrupt_check_cb = cb;
    _drawer.setInterruptCheckCallback(cb); // Pass to drawing module
}

bool DisplayListRenderer::isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const {
    if (!asset || asset->data.empty()) {
        return false;
    }
    for (uint8_t pixelValue : asset->data) {
        if (pixelValue == 0) { // 0 means transparent for DRAW
            return false;
        }
    }
    return true;
}

bool DisplayListRenderer::determineItemOpacity(const DisplayListItem& item) const {
    // FILL_* commands are always opaque over their shape.
    if (item.type == CMD_FILL_RECT || item.type == CMD_FILL_CIRCLE || item.type == CMD_FILL_PIXEL) {
        return true;
    }
    // PIXEL command draws a single scaled block of the current color. This is opaque.
    if (item.type == CMD_PIXEL) {
        return true;
    }
    // For DRAW commands, opacity depends on the asset being drawn.
    if (item.type == CMD_DRAW) {
        if (item.stringParams.count("NAME")) {
            String assetName = item.stringParams.at("NAME"); // Already uppercase
            if (_assets.count(assetName)) {
                return isAssetDataFullyOpaque(&_assets.at(assetName));
            }
        }
        return false; // No asset name or asset not found.
    }
    // LINE, RECT (outline), CIRCLE (outline) are not considered opaque for area culling.
    return false;
}


ScreenBounds DisplayListRenderer::calculateScreenBounds(const DisplayListItem& item) {
    ScreenBounds bounds;
    bounds.isOffScreen = true; // Default to off-screen

    float unclippedVisualMinX = 0, unclippedVisualMinY = 0, unclippedVisualMaxX = 0, unclippedVisualMaxY = 0;
    bool validShape = false;

    // Helper for transforming points using item's state
    // Now calls the public method on _drawer (which is MicroPatternsDrawing)
    auto transformItemPoint = [&](float lx, float ly, float& sx, float& sy) {
        _drawer.transformPoint(lx, ly, item.matrix, item.scaleFactor, sx, sy);
    };
    
    float s_center_x_for_circle = 0, s_center_y_for_circle = 0;
    float effective_radius_for_circle = 0;


    if (item.type == CMD_DRAW) {
        if (item.stringParams.count("NAME")) {
            String assetName = item.stringParams.at("NAME");
            if (_assets.count(assetName)) {
                const MicroPatternsAsset& asset = _assets.at(assetName);
                if (asset.width > 0 && asset.height > 0) {
                    int lx = item.intParams.at("X");
                    int ly = item.intParams.at("Y");
                    float s_pts_x[4], s_pts_y[4];
                    transformItemPoint(lx, ly, s_pts_x[0], s_pts_y[0]);
                    transformItemPoint(lx + asset.width, ly, s_pts_x[1], s_pts_y[1]);
                    transformItemPoint(lx + asset.width, ly + asset.height, s_pts_x[2], s_pts_y[2]);
                    transformItemPoint(lx, ly + asset.height, s_pts_x[3], s_pts_y[3]);

                    unclippedVisualMinX = std::min({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]});
                    unclippedVisualMinY = std::min({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]});
                    unclippedVisualMaxX = std::max({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]});
                    unclippedVisualMaxY = std::max({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]});
                    validShape = true;
                }
            }
        }
    } else if (item.type == CMD_RECT || item.type == CMD_FILL_RECT) {
        int lx = item.intParams.at("X");
        int ly = item.intParams.at("Y");
        int lw = item.intParams.at("WIDTH");
        int lh = item.intParams.at("HEIGHT");
        if (lw > 0 && lh > 0) {
            float s_pts_x[4], s_pts_y[4];
            transformItemPoint(lx, ly, s_pts_x[0], s_pts_y[0]);
            transformItemPoint(lx + lw, ly, s_pts_x[1], s_pts_y[1]);
            transformItemPoint(lx + lw, ly + lh, s_pts_x[2], s_pts_y[2]);
            transformItemPoint(lx, ly + lh, s_pts_x[3], s_pts_y[3]);
            unclippedVisualMinX = std::min({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]});
            unclippedVisualMinY = std::min({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]});
            unclippedVisualMaxX = std::max({s_pts_x[0], s_pts_x[1], s_pts_x[2], s_pts_x[3]});
            unclippedVisualMaxY = std::max({s_pts_y[0], s_pts_y[1], s_pts_y[2], s_pts_y[3]});
            validShape = true;
        }
    } else if (item.type == CMD_LINE) {
        float s_p1_x, s_p1_y, s_p2_x, s_p2_y;
        transformItemPoint(item.intParams.at("X1"), item.intParams.at("Y1"), s_p1_x, s_p1_y);
        transformItemPoint(item.intParams.at("X2"), item.intParams.at("Y2"), s_p2_x, s_p2_y);
        unclippedVisualMinX = std::min(s_p1_x, s_p2_x);
        unclippedVisualMinY = std::min(s_p1_y, s_p2_y);
        unclippedVisualMaxX = std::max(s_p1_x, s_p2_x);
        unclippedVisualMaxY = std::max(s_p1_y, s_p2_y);
        validShape = true;
    } else if (item.type == CMD_PIXEL || item.type == CMD_FILL_PIXEL) {
        float s_p_x, s_p_y;
        transformItemPoint(item.intParams.at("X"), item.intParams.at("Y"), s_p_x, s_p_y);
        // A pixel covers a 1x1 logical unit. Transform all 4 corners.
        float s_p1_x, s_p1_y, s_p2_x, s_p2_y, s_p3_x, s_p3_y, s_p4_x, s_p4_y;
        transformItemPoint(item.intParams.at("X"), item.intParams.at("Y"), s_p1_x, s_p1_y);
        transformItemPoint(item.intParams.at("X") + 1, item.intParams.at("Y"), s_p2_x, s_p2_y);
        transformItemPoint(item.intParams.at("X") + 1, item.intParams.at("Y") + 1, s_p3_x, s_p3_y);
        transformItemPoint(item.intParams.at("X"), item.intParams.at("Y") + 1, s_p4_x, s_p4_y);
        unclippedVisualMinX = std::min({s_p1_x, s_p2_x, s_p3_x, s_p4_x});
        unclippedVisualMinY = std::min({s_p1_y, s_p2_y, s_p3_y, s_p4_y});
        unclippedVisualMaxX = std::max({s_p1_x, s_p2_x, s_p3_x, s_p4_x});
        unclippedVisualMaxY = std::max({s_p1_y, s_p2_y, s_p3_y, s_p4_y});
        validShape = true;
    } else if (item.type == CMD_CIRCLE || item.type == CMD_FILL_CIRCLE) {
        int lcx = item.intParams.at("X");
        int lcy = item.intParams.at("Y");
        int lr = item.intParams.at("RADIUS");
        if (lr > 0) {
            transformItemPoint(lcx, lcy, s_center_x_for_circle, s_center_y_for_circle);
            float s_edge_on_x_axis_x, s_edge_on_x_axis_y;
            transformItemPoint(lcx + lr, lcy, s_edge_on_x_axis_x, s_edge_on_x_axis_y);
            float s_edge_on_y_axis_x, s_edge_on_y_axis_y;
            transformItemPoint(lcx, lcy + lr, s_edge_on_y_axis_x, s_edge_on_y_axis_y);
            
            float radius_proj_x = std::hypot(s_edge_on_x_axis_x - s_center_x_for_circle, s_edge_on_x_axis_y - s_center_y_for_circle);
            float radius_proj_y = std::hypot(s_edge_on_y_axis_x - s_center_x_for_circle, s_edge_on_y_axis_y - s_center_y_for_circle);
            effective_radius_for_circle = std::max({radius_proj_x, radius_proj_y, 1.0f});

            unclippedVisualMinX = s_center_x_for_circle - effective_radius_for_circle;
            unclippedVisualMaxX = s_center_x_for_circle + effective_radius_for_circle;
            unclippedVisualMinY = s_center_y_for_circle - effective_radius_for_circle;
            unclippedVisualMaxY = s_center_y_for_circle + effective_radius_for_circle;
            validShape = true;
        }
    }

    if (!validShape) {
        bounds.minX = bounds.minY = bounds.maxX = bounds.maxY = 0;
        bounds.markingBounds = {0,0,0,0};
        return bounds;
    }

    bounds.isOffScreen = (unclippedVisualMaxX <= 0 || unclippedVisualMinX >= _canvasWidth ||
                           unclippedVisualMaxY <= 0 || unclippedVisualMinY >= _canvasHeight);

    bounds.minX = static_cast<int>(floor(std::max(0.0f, unclippedVisualMinX)));
    bounds.minY = static_cast<int>(floor(std::max(0.0f, unclippedVisualMinY)));
    bounds.maxX = static_cast<int>(ceil(std::min(static_cast<float>(_canvasWidth), unclippedVisualMaxX)));
    bounds.maxY = static_cast<int>(ceil(std::min(static_cast<float>(_canvasHeight), unclippedVisualMaxY)));
    
    if (bounds.minX >= bounds.maxX || bounds.minY >= bounds.maxY) {
        bounds.isOffScreen = true;
    }

    // Default marking bounds to visual bounds
    bounds.markingBounds.minX = bounds.minX;
    bounds.markingBounds.minY = bounds.minY;
    bounds.markingBounds.maxX = bounds.maxX;
    bounds.markingBounds.maxY = bounds.maxY;

    // Adjust marking bounds for specific opaque shapes (similar to JS logic)
    if (item.isOpaque) {
        if (item.type == CMD_FILL_CIRCLE && validShape && effective_radius_for_circle > 0) {
            const float markingRadiusScaleFactor = sqrtf(M_PI / 4.0f); // approx 0.886
            const float markingRadius = effective_radius_for_circle * markingRadiusScaleFactor;
            bounds.markingBounds.minX = static_cast<int>(floor(std::max(0.0f, s_center_x_for_circle - markingRadius)));
            bounds.markingBounds.minY = static_cast<int>(floor(std::max(0.0f, s_center_y_for_circle - markingRadius)));
            bounds.markingBounds.maxX = static_cast<int>(ceil(std::min(static_cast<float>(_canvasWidth), s_center_x_for_circle + markingRadius)));
            bounds.markingBounds.maxY = static_cast<int>(ceil(std::min(static_cast<float>(_canvasHeight), s_center_y_for_circle + markingRadius)));
        } else if (item.type == CMD_FILL_RECT && validShape) {
            int lw = item.intParams.at("WIDTH");
            int lh = item.intParams.at("HEIGHT");
            if (lw > 0 && lh > 0) {
                const float matrixDeterminant = std::abs(item.matrix[0] * item.matrix[3] - item.matrix[1] * item.matrix[2]);
                const float actualScreenArea = static_cast<float>(lw) * lh * item.scaleFactor * item.scaleFactor * matrixDeterminant;
                const float visualAABBWidth = unclippedVisualMaxX - unclippedVisualMinX;
                const float visualAABBHeight = unclippedVisualMaxY - unclippedVisualMinY;
                const float visualAABBArea = visualAABBWidth * visualAABBHeight;

                if (actualScreenArea > 0 && visualAABBArea > 0) {
                    const float fillFactor = std::max(0.0f, std::min(1.0f, actualScreenArea / visualAABBArea));
                    if (fillFactor < 0.85f) { // Only adjust if significantly non-axis-aligned
                        const float scaleDownFactor = sqrtf(fillFactor);
                        const float markingWidth = visualAABBWidth * scaleDownFactor;
                        const float markingHeight = visualAABBHeight * scaleDownFactor;
                        const float centerX = (unclippedVisualMinX + unclippedVisualMaxX) / 2.0f;
                        const float centerY = (unclippedVisualMinY + unclippedVisualMaxY) / 2.0f;
                        bounds.markingBounds.minX = static_cast<int>(floor(std::max(0.0f, centerX - markingWidth / 2.0f)));
                        bounds.markingBounds.minY = static_cast<int>(floor(std::max(0.0f, centerY - markingHeight / 2.0f)));
                        bounds.markingBounds.maxX = static_cast<int>(ceil(std::min(static_cast<float>(_canvasWidth), centerX + markingWidth / 2.0f)));
                        bounds.markingBounds.maxY = static_cast<int>(ceil(std::min(static_cast<float>(_canvasHeight), centerY + markingHeight / 2.0f)));
                    }
                }
            }
        }
        // Ensure marking bounds are valid
        if (bounds.markingBounds.minX >= bounds.markingBounds.maxX || bounds.markingBounds.minY >= bounds.markingBounds.maxY) {
            // Fallback to visual bounds if marking bounds became invalid
            bounds.markingBounds.minX = bounds.minX;
            bounds.markingBounds.minY = bounds.minY;
            bounds.markingBounds.maxX = bounds.maxX;
            bounds.markingBounds.maxY = bounds.maxY;
        }
    }
    return bounds;
}


void DisplayListRenderer::renderItem(const DisplayListItem& item) {
    // Calls methods on _drawer
    switch (item.type) {
        case CMD_FILL_RECT:   _drawer.fillRect(item); break;
        case CMD_RECT:        _drawer.drawRect(item); break;
        case CMD_FILL_CIRCLE: _drawer.fillCircle(item); break;
        case CMD_CIRCLE:      _drawer.drawCircle(item); break;
        case CMD_LINE:        _drawer.drawLine(item); break;
        case CMD_PIXEL:       _drawer.drawPixel(item); break;
        // CMD_FILL_PIXEL was handled by drawPixel in MicroPatternsDrawing if fillAsset is considered.
        // If it's a distinct command type that MicroPatternsDrawing should handle, add it there.
        // For now, let's assume it's like CMD_PIXEL with fill.
        case CMD_FILL_PIXEL:  _drawer.drawPixel(item); break; // Or a specific fillPixel method if created
        case CMD_DRAW:
            if (item.stringParams.count("NAME")) {
                String assetName = item.stringParams.at("NAME"); // This is the key for _assets map
                if (_assets.count(assetName)) {
                    _drawer.drawBitmap(item, &_assets.at(assetName)); // Pass DisplayListItem and the resolved asset
                } else {
                    log_w("DisplayListRenderer (Line %d): Asset '%s' not found for DRAW.", item.sourceLine, assetName.c_str());
                }
            } else {
                 log_w("DisplayListRenderer (Line %d): DRAW command missing NAME parameter.", item.sourceLine);
            }
            break;
        // CMD_TEXT would be here if it's a display list item type
        // case CMD_TEXT: _drawer.drawText(item); break; 
        default:
            log_w("DisplayListRenderer (Line %d): Unknown item type %d for renderItem", item.sourceLine, (int)item.type);
    }
}

void DisplayListRenderer::render(const std::vector<DisplayListItem>& displayList) {
    _totalItems = displayList.size();
    _renderedItems = 0;
    _culledOffScreen = 0;
    _culledByOcclusion = 0;
    
    // _drawer.enablePixelOccupationMap(true); // Pixel occupation map logic removed from MicroPatternsDrawing for HAL refactor
    _occlusionBuffer.reset(); 
    _drawer.clearCanvas();   // Clear HAL's buffer via MicroPatternsDrawing

    // Iterate in reverse for back-to-front processing (last script command first).
    for (auto it = displayList.rbegin(); it != displayList.rend(); ++it) {
        if (_interrupt_check_cb && _interrupt_check_cb()) {
            log_i("DisplayListRenderer: Interrupt detected during rendering loop.");
            return; // Stop rendering
        }

        const DisplayListItem& item = *it;
        ScreenBounds bounds = calculateScreenBounds(item);

        if (bounds.isOffScreen) {
            _culledOffScreen++;
            continue;
        }
        
        // Check for zero-area bounds after clipping
        if (bounds.minX >= bounds.maxX || bounds.minY >= bounds.maxY) {
            _culledOffScreen++;
            continue;
        }

        if (item.isOpaque) { // Only check occlusion for opaque items
            // Use visual bounds for occlusion check
            if (_occlusionBuffer.isAreaOccluded(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY)) {
                _culledByOcclusion++;
                continue;
            }
        }

        renderItem(item);
        _renderedItems++;

        if (item.isOpaque) {
            // Use marking bounds to update occlusion buffer
            _occlusionBuffer.markAreaOpaque(bounds.markingBounds.minX, bounds.markingBounds.minY,
                                            bounds.markingBounds.maxX, bounds.markingBounds.maxY);
        }
    }
    log_i("Render complete: Total=%d, Rendered=%d, OffScreen=%d, Occluded=%d", // OverdrawSkippedPixels removed
          _totalItems, _renderedItems, _culledOffScreen, _culledByOcclusion);
    // _drawer.enablePixelOccupationMap(false); 
}
