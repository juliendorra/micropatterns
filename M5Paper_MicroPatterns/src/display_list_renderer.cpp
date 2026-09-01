#include "display_list_renderer.h"
#include "esp32-hal-log.h"
#include <algorithm> // For std::min, std::max
#include <cmath>     // For sqrtf, floor, ceil, round

DisplayListRenderer::DisplayListRenderer(MPCanvas* canvas,
                                       const std::map<String, MicroPatternsAsset>& assets,
                                       int canvasWidth, int canvasHeight)
    : _drawing(canvas),
      _occlusionBuffer(canvasWidth, canvasHeight, 16), // Default block size 16
      _canvasWidth(canvasWidth),
      _canvasHeight(canvasHeight),
      _totalItems(0), _renderedItems(0), _culledOffScreen(0), _culledByOcclusion(0),
      _interrupt_check_cb(nullptr) {
    (void)assets; // see the note on the removed _assets member in the header
    _drawing.setCanvas(canvas); // Ensure drawing module has the correct canvas
}

void DisplayListRenderer::setInterruptCheckCallback(std::function<bool()> cb) {
    _interrupt_check_cb = cb;
    _drawing.setInterruptCheckCallback(cb); // Pass to drawing module
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
    // For DRAW commands, opacity depends on the asset being drawn. The asset
    // pointer is resolved at display-list generation time, so no name lookup
    // happens here any more.
    if (item.type == CMD_DRAW) {
        return isAssetDataFullyOpaque(item.asset);
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
    auto transformItemPoint = [&](float lx, float ly, float& sx, float& sy) {
        _drawing.transformPoint(lx, ly, item, sx, sy);
    };
    
    float s_center_x_for_circle = 0, s_center_y_for_circle = 0;
    float effective_radius_for_circle = 0;


    if (item.type == CMD_DRAW) {
        if (item.asset) {
            {
                const MicroPatternsAsset& asset = *item.asset;
                if (asset.width > 0 && asset.height > 0) {
                    int lx = item.x();
                    int ly = item.y();
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
        int lx = item.x();
        int ly = item.y();
        int lw = item.w();
        int lh = item.h();
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
        transformItemPoint(item.x1(), item.y1(), s_p1_x, s_p1_y);
        transformItemPoint(item.x2(), item.y2(), s_p2_x, s_p2_y);
        unclippedVisualMinX = std::min(s_p1_x, s_p2_x);
        unclippedVisualMinY = std::min(s_p1_y, s_p2_y);
        unclippedVisualMaxX = std::max(s_p1_x, s_p2_x);
        unclippedVisualMaxY = std::max(s_p1_y, s_p2_y);
        validShape = true;
    } else if (item.type == CMD_PIXEL || item.type == CMD_FILL_PIXEL) {
        float s_p_x, s_p_y;
        transformItemPoint(item.x(), item.y(), s_p_x, s_p_y);
        // A pixel covers a 1x1 logical unit. Transform all 4 corners.
        float s_p1_x, s_p1_y, s_p2_x, s_p2_y, s_p3_x, s_p3_y, s_p4_x, s_p4_y;
        transformItemPoint(item.x(), item.y(), s_p1_x, s_p1_y);
        transformItemPoint(item.x() + 1, item.y(), s_p2_x, s_p2_y);
        transformItemPoint(item.x() + 1, item.y() + 1, s_p3_x, s_p3_y);
        transformItemPoint(item.x(), item.y() + 1, s_p4_x, s_p4_y);
        unclippedVisualMinX = std::min({s_p1_x, s_p2_x, s_p3_x, s_p4_x});
        unclippedVisualMinY = std::min({s_p1_y, s_p2_y, s_p3_y, s_p4_y});
        unclippedVisualMaxX = std::max({s_p1_x, s_p2_x, s_p3_x, s_p4_x});
        unclippedVisualMaxY = std::max({s_p1_y, s_p2_y, s_p3_y, s_p4_y});
        validShape = true;
    } else if (item.type == CMD_CIRCLE || item.type == CMD_FILL_CIRCLE) {
        int lcx = item.x();
        int lcy = item.y();
        int lr = item.radius();
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

    // The heuristic "marking bounds" that used to be computed here are GONE.
    //
    // They shrank an item's AABB to approximate the part of it that was solidly
    // painted -- a circle became a square of side r*sqrt(pi/4), a rotated rect
    // became its AABB scaled by sqrt(fillFactor). Both match the shape's AREA
    // rather than fitting INSIDE it: that square's corners sit at 1.25r, a
    // quarter-radius outside the circle. Unpainted pixels were marked opaque,
    // items behind them were culled, and their ink vanished from the render.
    //
    // Measured before removal, on the host corpus: 9 of 15 golden images
    // differed depending on whether culling was on, up to 2488 pixels. Occlusion
    // culling must be output-neutral -- it may only skip what is provably
    // covered -- so any such difference is a bug by definition.
    //
    // Marking now reads the drawing layer's occupancy bitmap instead, which is
    // exact and is what the web emulator has always done.

    return bounds;
}


void DisplayListRenderer::renderItem(const DisplayListItem& item) {
    // The _drawing methods now take DisplayListItem directly
    switch (item.type) {
        case CMD_FILL_RECT:   _drawing.fillRect(item); break;
        case CMD_RECT:        _drawing.drawRect(item); break;
        case CMD_FILL_CIRCLE: _drawing.fillCircle(item); break;
        case CMD_CIRCLE:      _drawing.drawCircle(item); break;
        case CMD_LINE:        _drawing.drawLine(item); break;
        case CMD_PIXEL:       _drawing.drawPixel(item); break;
        case CMD_FILL_PIXEL:  _drawing.drawFilledPixel(item); break;
        case CMD_DRAW:
            if (item.asset) {
                _drawing.drawAsset(item, *item.asset);
            } else {
                 log_w("DisplayListRenderer (Line %d): DRAW item has no resolved asset.", (int)item.sourceLine);
            }
            break;
        default:
            log_w("DisplayListRenderer (Line %d): Unknown item type %d", (int)item.sourceLine, (int)item.type);
    }
}

void DisplayListRenderer::render(const std::vector<DisplayListItem>& displayList) {
    _totalItems = displayList.size();
    _renderedItems = 0;
    _culledOffScreen = 0;
    _culledByOcclusion = 0;
    
    _drawing.enablePixelOccupationMap(true); // Enable for this render pass
    _occlusionBuffer.reset(); // Reset occlusion buffer state
    _drawing.clearCanvas();   // Clear canvas to white (this will also call _drawing.resetPixelOccupationMap())

    // INTERRUPT GRANULARITY -- what actually bounds abort latency.
    //
    // The check below is per display-list item, but that is NOT the binding
    // constraint: MicroPatternsDrawing polls the same callback once per SCANLINE
    // inside fillRect(), fillCircle() and drawAsset(), which is where a heavy
    // script spends essentially all of its rasterization time. Measured on the
    // host harness across the whole corpus, the worst gap between two consecutive
    // interrupt checks during a complete render is 0.3%-0.8% of that render --
    // i.e. ~25-65 ms on an 8 s device render.
    //
    // The genuinely coarse cases are the primitives with no inner check at all:
    // PIXEL / FILL_PIXEL (screen bounding box grows with SCALE^2, up to full
    // screen) and LINE / RECT / CIRCLE (bounded by perimeter). A synthetic script
    // of large PIXEL items records exactly one check per item, and its worst gap
    // is 7% of the render. Closing that needs a per-scanline check in
    // micropatterns_drawing.cpp's drawPixel()/drawFilledPixel(); it cannot be done
    // from here, because each primitive computes its own screen bounds.
    for (auto it = displayList.rbegin(); it != displayList.rend(); ++it) {
        if (_interrupt_check_cb && _interrupt_check_cb()) {
            log_i("DisplayListRenderer: Interrupt detected during rendering loop.");
            _drawing.enablePixelOccupationMap(false); // do not leave the pass flag set
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

        if (_occlusionEnabled && item.isOpaque) { // Only check occlusion for opaque items
            // Use visual bounds for occlusion check
            if (_occlusionBuffer.isAreaOccluded(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY)) {
                _culledByOcclusion++;
                continue;
            }
        }

        renderItem(item);
        _renderedItems++;

        if (_occlusionEnabled && item.isOpaque) {
            // Mark from the pixels this item ACTUALLY painted, not from an
            // estimate of its shape. See OcclusionBuffer::updateFromPixelMap().
            _occlusionBuffer.updateFromPixelMap(bounds.minX, bounds.minY,
                                                bounds.maxX, bounds.maxY,
                                                _drawing.occupancyBase(),
                                                _drawing.occStride());
        }
    }
    log_i("Render complete: Total=%d, Rendered=%d, OffScreen=%d, Occluded=%d, OverdrawSkippedPixels=%u",
          _totalItems, _renderedItems, _culledOffScreen, _culledByOcclusion, _drawing.getOverdrawSkippedPixelsCount());
    _drawing.enablePixelOccupationMap(false); // Disable after render pass (optional, good practice)
}