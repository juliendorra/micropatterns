#include "occlusion_buffer.h"
#include <algorithm> // For std::min, std::max
#include "esp32-hal-log.h" // For logging (optional)

OcclusionBuffer::OcclusionBuffer(int canvasWidth, int canvasHeight, int blockSize)
    : _canvasWidth(canvasWidth), _canvasHeight(canvasHeight), _blockSize(blockSize), _culledByOcclusionCount(0) {
    if (_blockSize <= 0) _blockSize = 1; // Ensure block size is positive
    _gridWidth = (_canvasWidth + _blockSize - 1) / _blockSize; // Ceiling division
    _gridHeight = (_canvasHeight + _blockSize - 1) / _blockSize; // Ceiling division
    _grid.resize(_gridWidth * _gridHeight, 0);
    // log_d("OcclusionBuffer created: Canvas(%dx%d), BlockSize(%d), Grid(%dx%d)", _canvasWidth, _canvasHeight, _blockSize, _gridWidth, _gridHeight);
}

void OcclusionBuffer::reset() {
    std::fill(_grid.begin(), _grid.end(), 0);
    _culledByOcclusionCount = 0;
    // log_d("OcclusionBuffer reset.");
}

OcclusionBuffer::GridIndices OcclusionBuffer::_getGridIndices(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY) const {
    GridIndices indices;
    indices.startCol = std::max(0, screenMinX / _blockSize);
    indices.endCol = std::min(_gridWidth - 1, (screenMaxX -1) / _blockSize); // Use maxX-1 for end col
    indices.startRow = std::max(0, screenMinY / _blockSize);
    indices.endRow = std::min(_gridHeight - 1, (screenMaxY-1) / _blockSize); // Use maxY-1 for end row
    return indices;
}

void OcclusionBuffer::markAreaOpaque(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY) {
    if (screenMinX >= screenMaxX || screenMinY >= screenMaxY) return; // Invalid area

    GridIndices gi = _getGridIndices(screenMinX, screenMinY, screenMaxX, screenMaxY);
    // log_d("Marking opaque: ScreenRect (%d,%d)-(%d,%d) -> GridRect (%d,%d)-(%d,%d)", screenMinX, screenMinY, screenMaxX, screenMaxY, gi.startCol, gi.startRow, gi.endCol, gi.endRow);

    for (int r = gi.startRow; r <= gi.endRow; ++r) {
        for (int c = gi.startCol; c <= gi.endCol; ++c) {
            if (r >= 0 && r < _gridHeight && c >= 0 && c < _gridWidth) { // Bounds check for safety
                 _grid[r * _gridWidth + c] = 1; // Mark as opaque
            }
        }
    }
}

bool OcclusionBuffer::isAreaOccluded(int screenMinX, int screenMinY, int screenMaxX, int screenMaxY) const {
    if (screenMinX >= screenMaxX || screenMinY >= screenMaxY) return false; // Invalid or zero-size area cannot be occluded

    GridIndices gi = _getGridIndices(screenMinX, screenMinY, screenMaxX, screenMaxY);
    // log_d("Checking occlusion: ScreenRect (%d,%d)-(%d,%d) -> GridRect (%d,%d)-(%d,%d)", screenMinX, screenMinY, screenMaxX, screenMaxY, gi.startCol, gi.startRow, gi.endCol, gi.endRow);

    for (int r = gi.startRow; r <= gi.endRow; ++r) {
        for (int c = gi.startCol; c <= gi.endCol; ++c) {
             if (r >= 0 && r < _gridHeight && c >= 0 && c < _gridWidth) { // Bounds check
                if (_grid[r * _gridWidth + c] == 0) {
                    // log_d("Area not occluded: Grid cell (%d,%d) is transparent.", c, r);
                    return false; // Found a transparent block, so not fully occluded
                }
            } else { // Should not happen if _getGridIndices is correct
                // log_w("Occlusion check: Index out of bounds (%d,%d) for grid (%dx%d)", c, r, _gridWidth, _gridHeight);
                return false; // Treat out-of-bounds as not occluded for safety
            }
        }
    }
    // log_d("Area IS occluded.");
    _culledByOcclusionCount++;
    return true; // All blocks covered by the area are opaque
}
