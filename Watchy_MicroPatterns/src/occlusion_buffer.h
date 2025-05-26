#ifndef OCCLUSION_BUFFER_H
#define OCCLUSION_BUFFER_H

#include <vector>
#include <Arduino.h> // For uint8_t

class OcclusionBuffer {
public:
    OcclusionBuffer(int canvasWidth, int canvasHeight, int blockSize = 16);

    void reset();
    // Marks the grid cells covered by the screen-space AABB as opaque.
    void markAreaOpaque(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY);
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
