#ifndef OCCLUSION_BUFFER_H
#define OCCLUSION_BUFFER_H

#include <vector>
#include <Arduino.h> // For uint8_t

class OcclusionBuffer {
public:
    OcclusionBuffer(int canvasWidth, int canvasHeight, int blockSize = 16);

    void reset();
    // Marks the grid cells covered by the screen-space AABB as opaque.
    //
    // UNSOUND unless the caller can guarantee every pixel in that box was
    // painted. Kept for callers that can; the renderer no longer uses it --
    // see updateFromPixelMap().
    void markAreaOpaque(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY);

    // Marks a grid cell opaque only if EVERY pixel in it was actually painted,
    // read from the drawing layer's 1-bit occupancy map.
    //
    // This replaces marking from geometric bounds, which was unsound: the
    // shrink factors used there matched the shape's AREA rather than fitting
    // inside it (a circle was marked with a square of side r*sqrt(pi/4), whose
    // corners sit at 1.25r -- outside the circle). Pixels that were never
    // painted got marked opaque, later items behind them were culled, and their
    // ink disappeared. The web emulator has always done it this way; this is
    // the C++ side catching up, and it makes culling output-neutral by
    // construction rather than by heuristic.
    //
    // `map` is the drawing layer's occupancy bitmap, 1 bit per pixel, `stride`
    // bytes per row. Pass nullptr when no map is live: nothing is marked, which
    // costs culling and is always safe.
    void updateFromPixelMap(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY,
                            const uint8_t* map, int stride);
    // Checks if all grid cells for the screen-space AABB are already marked opaque.
    bool isAreaOccluded(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY) const;

    int getCulledByOcclusionCount() const { return _culledByOcclusionCount; }

private:
    int _canvasWidth;
    int _canvasHeight;
    int _blockSize;
    int _gridWidth;
    int _gridHeight;
    std::vector<uint8_t> _grid; // 0 = empty/transparent, 1 = opaque
    mutable int _culledByOcclusionCount; // Track items culled by this buffer

    struct GridIndices {
        int startCol, endCol, startRow, endRow;
    };
    GridIndices _getGridIndices(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY) const;
};

#endif // OCCLUSION_BUFFER_H