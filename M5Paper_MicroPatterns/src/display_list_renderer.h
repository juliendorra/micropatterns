#ifndef DISPLAY_LIST_RENDERER_H
#define DISPLAY_LIST_RENDERER_H

#include <vector>
#include <map>
#include "micropatterns_command.h" // For DisplayListItem, MicroPatternsAsset
#include "micropatterns_drawing.h"
#include "occlusion_buffer.h"
#include "mp_canvas.h" // MPCanvas -- the renderer needs a canvas, not a DisplayManager

struct ScreenBounds {
    int minX, minY, maxX, maxY;
    bool isOffScreen;
    struct { // For occlusion buffer marking, potentially smaller than visual bounds
        int minX, minY, maxX, maxY;
    } markingBounds;
};

class DisplayListRenderer {
public:
    // Takes the canvas directly. It previously took a DisplayManager& purely to
    // call getCanvas() once in the initializer list -- the member was stored and
    // then never read again, which coupled the renderer to M5Paper-only display
    // code for no benefit. Passing MPCanvas* makes it platform-agnostic.
    DisplayListRenderer(MPCanvas* canvas,
                        const std::map<String, MicroPatternsAsset>& assets,
                        int canvasWidth, int canvasHeight);

    void render(const std::vector<DisplayListItem>& displayList);

    // Stats (optional)
    int getTotalItems() const { return _totalItems; }
    int getRenderedItems() const { return _renderedItems; }
    int getCulledOffScreen() const { return _culledOffScreen; }
    int getCulledByOcclusion() const { return _culledByOcclusion; }
    // Pixels the drawing layer skipped because the pixel-occupation map said
    // they were already covered. Already tracked by MicroPatternsDrawing and
    // already logged by render(); this getter just exposes it to callers
    // (used by tools/host_harness). Purely additive, not called during render.
    unsigned int getOverdrawSkippedPixels() const { return _drawing.getOverdrawSkippedPixelsCount(); }

    void setInterruptCheckCallback(std::function<bool()> cb);


private:
    MicroPatternsDrawing _drawing;
    const std::map<String, MicroPatternsAsset>& _assets; // Reference to assets from parser
    OcclusionBuffer _occlusionBuffer;
    
    int _canvasWidth;
    int _canvasHeight;

    // Stats
    int _totalItems;
    int _renderedItems;
    int _culledOffScreen;
    int _culledByOcclusion; // Items culled by occlusion buffer

    std::function<bool()> _interrupt_check_cb;


    ScreenBounds calculateScreenBounds(const DisplayListItem& item);
    void renderItem(const DisplayListItem& item);
    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
    bool determineItemOpacity(const DisplayListItem& item) const;
};

#endif // DISPLAY_LIST_RENDERER_H