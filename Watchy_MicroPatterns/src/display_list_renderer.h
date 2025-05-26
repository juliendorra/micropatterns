#ifndef DISPLAY_LIST_RENDERER_H
#define DISPLAY_LIST_RENDERER_H

#include <vector>
#include <map>
#include "micropatterns_command.h" // For DisplayListItem, MicroPatternsAsset
#include "micropatterns_drawing.h"
#include "occlusion_buffer.h"
// #include "display_manager.h" // No longer needed directly

struct ScreenBounds {
    int minX, minY, maxX, maxY;
    bool isOffScreen;
    struct { // For occlusion buffer marking, potentially smaller than visual bounds
        int minX, minY, maxX, maxY;
    } markingBounds;
};

class DisplayListRenderer {
public:
    // Constructor now takes MicroPatternsDrawing reference
    DisplayListRenderer(MicroPatternsDrawing& drawer,
                        const std::map<String, MicroPatternsAsset>& assets,
                        int canvasWidth, int canvasHeight);

    void render(const std::vector<DisplayListItem>& displayList);

    // Stats (optional)
    int getTotalItems() const { return _totalItems; }
    int getRenderedItems() const { return _renderedItems; }
    int getCulledOffScreen() const { return _culledOffScreen; }
    int getCulledByOcclusion() const { return _culledByOcclusion; }

    void setInterruptCheckCallback(std::function<bool()> cb);


private:
    // DisplayManager& _displayMgr; // Removed
    MicroPatternsDrawing& _drawer; // Changed from object to reference
    const std::map<String, MicroPatternsAsset>& _assets; // Reference to assets from parser
    OcclusionBuffer _occlusionBuffer;
    
    int _canvasWidth; // Still needed for OcclusionBuffer
    int _canvasHeight;

    // Stats
    int _totalItems;
    int _renderedItems;
    int _culledOffScreen;
    int _culledByOcclusion; // Items culled by occlusion buffer

    std::function<bool()> _interrupt_check_cb;


    ScreenBounds calculateScreenBounds(const DisplayListItem& item); // Will use _drawer for transformations if needed
    void renderItem(const DisplayListItem& item); // Will call _drawer methods
    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
    bool determineItemOpacity(const DisplayListItem& item) const; // Logic based on item type and asset properties
};

#endif // DISPLAY_LIST_RENDERER_H
