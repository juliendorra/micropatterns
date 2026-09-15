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
    DisplayListRenderer(MPCanvas* canvas, int canvasWidth, int canvasHeight);

    void render(const std::vector<DisplayListItem>& displayList);

    // Stats (optional)
    int getTotalItems() const { return _totalItems; }
    int getRenderedItems() const { return _renderedItems; }
    int getCulledOffScreen() const { return _culledOffScreen; }
    int getCulledByOcclusion() const { return _culledByOcclusion; }
    bool usedOccupancyMapLastRender() const { return _usedOccupancyMapLastRender; }

    // Occlusion culling on/off. On by default -- this exists so the host
    // harness can render the same script both ways and byte-compare the
    // result. Culling is meant to be output-NEUTRAL: it only skips items it
    // has proven are completely covered by opaque items drawn later. If the
    // two renders ever differ, the culling is wrong, and that is a silent
    // class of bug no golden image can catch on its own.
    void setOcclusionEnabled(bool on) { _occlusionEnabled = on; }

    // Simulates the device failing to allocate the pixel occupancy map, which
    // the Watchy really does when the heap is fragmented (see
    // MicroPatternsDrawing::initPixelOccupationMap). Exists so the harness can
    // check what that costs -- the comment there claims "speed, not
    // correctness", and this is how that claim gets tested rather than trusted.
    void setOccupancyMapEnabled(bool on) { _occupancyMapEnabled = on; }

    // Q16.16 fixed-point pattern-fill coordinates and the screen-space circle
    // span. ON by default. Setting it false gives the original float
    // rasteriser, kept selectable so `compare-paths displaylist
    // displaylist-float` can still measure the distance between the two --
    // the practice commit d427b02 established.
    void setFixedPointEnabled(bool on) { _fixedPointEnabled = on; }
    void setIntegerTransformEnabled(bool on) { _integerTransformEnabled = on; }
    void setSpanWriterEnabled(bool on) { _spanWriterEnabled = on; }
    void setIntegerDdaEnabled(bool on) { _integerDdaEnabled = on; }
    unsigned long getIntDdaRows() const { return _drawing.getIntDdaRows(); }
    unsigned long getFloatFallbackRows() const { return _drawing.getFloatFallbackRows(); }
    unsigned long getSpanRows() const { return _drawing.getSpanRows(); }
    unsigned long getTiledRows() const { return _drawing.getTiledRows(); }
    unsigned long getClippedRows() const { return _drawing.getClippedRows(); }

    // How many pixels the last render emitted through a fixed-point inner loop.
    // Zero from a path that claims to be fixed-point means it never ran; see
    // micropatterns_drawing.h for why this is a gate and not a statistic.
    unsigned long getFixedPointPixels() const { return _drawing.getFixedPointPixels(); }
    unsigned long getIntegerXformCalls() const { return _drawing.getIntegerXformCalls(); }

#if MP_PROFILE_ITEMS
    // Per-item-type wall clock, for answering "where does this script actually
    // spend its time" instead of guessing. Compiled out entirely unless the
    // build asks for it: it takes a timestamp around every item, which is
    // cheap next to a filled circle and NOT cheap next to a PIXEL, so the
    // numbers it produces are a distribution, not an absolute cost.
    // Must cover the whole CommandType enum, which runs to CMD_NOOP = 24.
    // It was 16, which silently dropped CMD_CIRCLE (16) and CMD_FILL_CIRCLE
    // (17) -- so a circle-heavy script reported 73% of its time "unaccounted"
    // and very nearly got a headline saying the time was outside drawing.
    // The bug was in the instrument, as it usually is here.
    static const int kProfileTypes = 32;
    int64_t profUs[kProfileTypes] = {0};
    int32_t profCount[kProfileTypes] = {0};
    int64_t profOverheadUs = 0;
    void resetProfile() {
        for (int i = 0; i < kProfileTypes; ++i) { profUs[i] = 0; profCount[i] = 0; }
        profOverheadUs = 0;
    }
#endif
    // Pixels the drawing layer skipped because the pixel-occupation map said
    // they were already covered. Already tracked by MicroPatternsDrawing and
    // already logged by render(); this getter just exposes it to callers
    // (used by tools/host_harness). Purely additive, not called during render.
    unsigned int getOverdrawSkippedPixels() const { return _drawing.getOverdrawSkippedPixelsCount(); }

    void setInterruptCheckCallback(std::function<bool()> cb);


private:
    MicroPatternsDrawing _drawing;
    // DRAW items carry a resolved MicroPatternsAsset* (owned by the MpProgram)
    // from display-list generation, so the renderer holds no asset table.
    OcclusionBuffer _occlusionBuffer;
    
    int _canvasWidth;
    int _canvasHeight;

    // Stats
    int _totalItems;
    int _renderedItems;
    int _culledOffScreen;
    int _culledByOcclusion; // Items culled by occlusion buffer
    bool _occlusionEnabled = true;
    bool _occupancyMapEnabled = true;
    bool _fixedPointEnabled = true;
    bool _integerTransformEnabled = true;
    bool _spanWriterEnabled = true;
    bool _integerDdaEnabled = true;
    bool _usedOccupancyMapLastRender = false;

    std::function<bool()> _interrupt_check_cb;


    ScreenBounds calculateScreenBounds(const DisplayListItem& item);
    // The same bounds from the exact integer transform, in Q15, with no float:
    // floor and ceil are shifts. Used when the integer walk is enabled so the
    // bounds pass agrees with the rasteriser it feeds, and so no float remains
    // between a display list and the pixels.
    ScreenBounds calculateScreenBoundsQ15(const DisplayListItem& item);
    void renderItem(const DisplayListItem& item);
    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
    bool determineItemOpacity(const DisplayListItem& item) const;
};

#endif // DISPLAY_LIST_RENDERER_H
