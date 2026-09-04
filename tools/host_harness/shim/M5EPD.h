// Host shim for <M5EPD.h>.
// Part of tools/host_harness. NOT used by the firmware build.
//
// Provides a host-backed M5EPD_Canvas with the exact surface the
// platform-agnostic renderer core uses: width/height/drawPixel/fillCanvas/
// readPixel/createCanvas. The pixel store is packed 4bpp, matching the
// M5Paper's grayscale canvas layout, so drawPixel keeps its read-modify-write
// nibble cost rather than degenerating into a byte store.
#ifndef HOST_SHIM_M5EPD_H
#define HOST_SHIM_M5EPD_H

#include <Arduino.h>
#include <cstdint>
#include <vector>

// Update modes are never exercised on the host; the enum exists so firmware
// headers that mention it parse.
typedef enum {
    UPDATE_MODE_INIT = 0,
    UPDATE_MODE_DU,
    UPDATE_MODE_GC16,
    UPDATE_MODE_GL16,
    UPDATE_MODE_GLR16,
    UPDATE_MODE_GLD16,
    UPDATE_MODE_DU4,
    UPDATE_MODE_A2,
    UPDATE_MODE_NONE
} m5epd_update_mode_t;

#define MP_HOST_SHIM_HAS_FILL_MASK_ROW 1
class M5EPD_Canvas;
inline void mp_canvas_fill_mask_row(M5EPD_Canvas* c, int y, int byteX0, int nBytes,
                                    const uint8_t* cover, uint32_t color);

class M5EPD_Canvas {
public:
    M5EPD_Canvas() : _w(0), _h(0) {}
    explicit M5EPD_Canvas(void* /*driver*/) : _w(0), _h(0) {}

    void* createCanvas(int16_t width, int16_t height, uint8_t frames = 1) {
        (void)frames;
        _w = width;
        _h = height;
        _buf.assign(((size_t)_w * _h + 1) / 2, 0x00);
        return _buf.empty() ? nullptr : (void*)_buf.data();
    }

    int32_t width() const { return _w; }
    int32_t height() const { return _h; }

    void fillCanvas(uint32_t color) {
        uint8_t c = (uint8_t)(color & 0x0F);
        uint8_t packed = (uint8_t)((c << 4) | c);
        std::fill(_buf.begin(), _buf.end(), packed);
    }

    void drawPixel(int32_t x, int32_t y, uint32_t color) {
        if (x < 0 || y < 0 || x >= _w || y >= _h) return;
        size_t idx = (size_t)y * _w + (size_t)x;
        uint8_t& b = _buf[idx >> 1];
        uint8_t c = (uint8_t)(color & 0x0F);
        if (idx & 1) b = (uint8_t)((b & 0xF0) | c);
        else         b = (uint8_t)((b & 0x0F) | (c << 4));
    }

    uint16_t readPixel(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || x >= _w || y >= _h) return 0;
        size_t idx = (size_t)y * _w + (size_t)x;
        uint8_t b = _buf[idx >> 1];
        return (uint16_t)((idx & 1) ? (b & 0x0F) : (b >> 4));
    }

    // Row blit from a MSB-first bit mask; see mp_canvas.h. Nibble-wise, so the
    // host exercises the real span path rather than a fallback -- the
    // equivalence gate is only worth something if this is the code under test.
    void fillMaskRow(int y, int byteX0, int nBytes, const uint8_t* cover, uint32_t color) {
        if (y < 0 || y >= _h) return;
        const uint8_t c = (uint8_t)(color & 0x0F);
        for (int i = 0; i < nBytes; ++i) {
            uint8_t m = cover[i];
            if (!m) continue;
            const int xb = (byteX0 + i) << 3;
            for (int k = 0; k < 8; ++k) {
                if (!(m & (0x80u >> k))) continue;
                const int x = xb + k;
                if (x >= _w) break;
                size_t idx = (size_t)y * _w + (size_t)x;
                uint8_t& b = _buf[idx >> 1];
                if (idx & 1) b = (uint8_t)((b & 0xF0) | c);
                else         b = (uint8_t)((b & 0x0F) | (c << 4));
            }
        }
    }

    // --- host-only accessors (not part of the M5EPD API) -------------------
    const std::vector<uint8_t>& hostBuffer() const { return _buf; }

private:
    int32_t _w;
    int32_t _h;
    std::vector<uint8_t> _buf; // packed 4bpp, two pixels per byte, high nibble first
};


inline void mp_canvas_fill_mask_row(M5EPD_Canvas* c, int y, int byteX0, int nBytes,
                                    const uint8_t* cover, uint32_t color) {
    c->fillMaskRow(y, byteX0, nBytes, cover, color);
}

#endif // HOST_SHIM_M5EPD_H
