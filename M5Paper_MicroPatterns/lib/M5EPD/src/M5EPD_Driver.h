#ifndef _M5EPD_DRIVER_H_
#define _M5EPD_DRIVER_H_

#include <Arduino.h>
#include <SPI.h>

#include "utility/IT8951_Defines.h"

#define M5EPD_PANEL_W 960
#define M5EPD_PANEL_H 540

// ===========================================================================
// MicroPatterns LOCAL MODIFICATION -- tunables for the patched transfer path.
// See lib/M5EPD/README-VENDORED.md for the full rationale.
// ===========================================================================

// SPI clock for every host->IT8951 transfer.
//
// Upstream hard-coded 10 MHz in two places with no comment explaining the
// choice, and NO IT8951 datasheet maximum could be found. It is deliberately
// left at 10 MHz here: raising it is a ONE-LINE change, but it is an
// EMPIRICAL one that must be validated on the actual panel.
//
// FAILURE MODE IF TOO HIGH: image corruption -- torn/garbled bands, shifted
// pixels, or a screen of noise -- because the controller mis-samples MOSI.
// It will NOT present as a blank screen or a hang. If you raise this and the
// panel goes garbage, put it back.
#define M5EPD_SPI_FREQ_HZ 10000000

// Staging buffer for the bulk SPI path (colour inversion / constant fill).
// Must be a multiple of 4. Lives inside the driver object, in internal DRAM.
#define M5EPD_SPI_CHUNK_BYTES 2048

// --- 1bpp display-mode parameters ------------------------------------------

// Alignment required of the panel-native X and W of a 1bpp update.
//
// Packing alone only needs 8. Waveshare's notes (and several IT8951 bug
// reports) say some LUT versions need X and W 32-pixel aligned or NOTHING is
// displayed. We ASSUME that applies and enforce 32; our only real caller is
// full-panel (x=0, w=960) which satisfies it either way. If you ever need an
// unaligned window and are confident the LUT does not care, lower this to 8.
#define M5EPD_1BPP_ALIGN 32

// IT8951 BGVR (0x1000+0x250) colour table: bit[15:8] = grey shown for a SET
// bit, bit[7:0] = grey shown for a CLEAR bit. Values are 8bpp greys.
// We pack "1 = white", matching Waveshare's EPD_IT8951_Display_1bp defaults.
// FAILURE MODE IF SWAPPED: a perfectly sharp but INVERTED image. Swap these
// two constants to fix; do not go hunting in the packing code.
#define M5EPD_1BPP_FRONT_GREY 0xF0
#define M5EPD_1BPP_BACK_GREY  0x00

// Endianness declared to LD_IMG_AREA for the 1bpp (packed-as-8bpp) load.
// The working 4bpp path ships buffer bytes in order under B_ENDIAN, and we
// ship bytes in order too, so B_ENDIAN is the consistent choice. (Waveshare
// declares L_ENDIAN because their SPI layer byte-swaps each 16-bit word on
// the way out; the net wire order is the same.)
// FAILURE MODE IF WRONG: every pair of bytes swapped -> the image reads as
// 8-pixel-wide columns transposed in pairs. Flip to IT8951_LDIMG_L_ENDIAN.
#define M5EPD_1BPP_ENDIAN IT8951_LDIMG_B_ENDIAN

typedef enum {
    M5EPD_OK = 0,
    M5EPD_BUSYTIMEOUT,
    M5EPD_OUTOFBOUNDS,
    M5EPD_NOTMULTIPLE4,
    M5EPD_OTHERERR
} m5epd_err_t;

/*
---------------------------------------- Refresh mode description
---------------------------------------- INIT The initialization (INIT) mode is
used to completely erase the display and leave it in the white state. It is
useful for situations where the display information in memory is not a faithful
representation of the optical state of the display, for example, after the
device receives power after it has been fully powered down. This waveform
switches the display several times and leaves it in the white state.

DU
The direct update (DU) is a very fast, non-flashy update. This mode supports
transitions from any graytone to black or white only. It cannot be used to
update to any graytone other than black or white. The fast update time for this
mode makes it useful for response to touch sensor or pen input or menu selection
indictors.

GC16
The grayscale clearing (GC16) mode is used to update the full display and
provide a high image quality. When GC16 is used with Full Display Update the
entire display will update as the new image is written. If a Partial Update
command is used the only pixels with changing graytone values will update. The
GC16 mode has 16 unique gray levels.

GL16
The GL16 waveform is primarily used to update sparse content on a white
background, such as a page of anti-aliased text, with reduced flash. The GL16
waveform has 16 unique gray levels.

GLR16
The GLR16 mode is used in conjunction with an image preprocessing algorithm to
update sparse content on a white background with reduced flash and reduced image
artifacts. The GLR16 mode supports 16 graytones. If only the even pixel states
are used (0, 2, 4, … 30), the mode will behave exactly as a traditional GL16
waveform mode. If a separately-supplied image preprocessing algorithm is used,
the transitions invoked by the pixel states 29 and 31 are used to improve
display quality. For the AF waveform, it is assured that the GLR16 waveform data
will point to the same voltage lists as the GL16 data and does not need to be
stored in a separate memory.

GLD16
The GLD16 mode is used in conjunction with an image preprocessing algorithm to
update sparse content on a white background with reduced flash and reduced image
artifacts. It is recommended to be used only with the full display update. The
GLD16 mode supports 16 graytones. If only the even pixel states are used (0, 2,
4, … 30), the mode will behave exactly as a traditional GL16 waveform mode. If a
separately-supplied image preprocessing algorithm is used, the transitions
invoked by the pixel states 29 and 31 are used to refresh the background with a
lighter flash compared to GC16 mode following a predetermined pixel map as
encoded in the waveform file, and reduce image artifacts even more compared to
the GLR16 mode. For the AF waveform, it is assured that the GLD16 waveform data
will point to the same voltage lists as the GL16 data and does not need to be
stored in a separate memory.

DU4
The DU4 is a fast update time (similar to DU), non-flashy waveform. This mode
supports transitions from any gray tone to gray tones 1,6,11,16 represented by
pixel states [0 10 20 30]. The combination of fast update time and four gray
tones make it useful for anti-aliased text in menus. There is a moderate
increase in ghosting compared with GC16.

A2
The A2 mode is a fast, non-flash update mode designed for fast paging turning or
simple black/white animation. This mode supports transitions from and to black
or white only. It cannot be used to update to any graytone other than black or
white. The recommended update sequence to transition into repeated A2 updates is
shown in Figure 1. The use of a white image in the transition from 4-bit to
1-bit images will reduce ghosting and improve image quality for A2 updates.

*/

typedef enum               //             Typical
{                          //   Ghosting  Update Time  Usage
    UPDATE_MODE_INIT = 0,  // * N/A       2000ms       Display initialization,
    UPDATE_MODE_DU   = 1,  //   Low       260ms        Monochrome menu, text
                           //   input, and touch screen input
    UPDATE_MODE_GC16 = 2,  // * Very Low  450ms        High quality images
    UPDATE_MODE_GL16 =
        3,  // * Medium    450ms        Text with white background
    UPDATE_MODE_GLR16 =
        4,  //   Low       450ms        Text with white background
    UPDATE_MODE_GLD16 =
        5,  //   Low       450ms        Text and graphics with white background
    UPDATE_MODE_DU4 =
        6,  // * Medium    120ms        Fast page flipping at reduced contrast
    UPDATE_MODE_A2 = 7,  //   Medium    290ms        Anti-aliased text in menus
                         //   / touch and screen input
    UPDATE_MODE_NONE = 8
} m5epd_update_mode_t;  // The ones marked with * are more commonly used

class M5EPD_Driver {
   public:
    static const uint8_t ROTATE_0   = IT8951_ROTATE_0;
    static const uint8_t ROTATE_90  = IT8951_ROTATE_90;
    static const uint8_t ROTATE_180 = IT8951_ROTATE_180;
    static const uint8_t ROTATE_270 = IT8951_ROTATE_270;

   public:
    M5EPD_Driver(int8_t spi_index = VSPI);
    ~M5EPD_Driver();
    m5epd_err_t begin(int8_t sck, int8_t mosi, int8_t miso, int8_t cs,
                      int8_t busy, int8_t rst = -1);
    m5epd_err_t Clear(bool init = false);
    m5epd_err_t WriteFullGram4bpp(const uint8_t *gram);
    m5epd_err_t WritePartGram4bpp(uint16_t x, uint16_t y, uint16_t w,
                                  uint16_t h, const uint8_t *gram);
    m5epd_err_t FillPartGram4bpp(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint16_t data);

    // --- MicroPatterns LOCAL ADDITION: 1bpp fast path ----------------------
    //
    // WARNING: unlike every other method on this class, px/py/pw/ph here are
    // PANEL-NATIVE (rotate-0) coordinates, NOT canvas coordinates. The
    // IT8951 applies SetRotation() while it LOADS an image, which is
    // incoherent with 8-pixels-per-byte packing (it would rotate whole bytes,
    // not pixels). So the caller must have already transposed the pixels into
    // panel orientation; M5EPD_Canvas::pushCanvas1bpp() does exactly that.
    //
    // `packed` is 8 pixels per byte, MSB = leftmost panel pixel, row stride
    // pw/8 bytes, ph rows. A SET bit shows M5EPD_1BPP_FRONT_GREY.
    // px and pw must be multiples of M5EPD_1BPP_ALIGN.
    m5epd_err_t Write1bppGram(uint16_t px, uint16_t py, uint16_t pw,
                              uint16_t ph, const uint8_t *packed);

    // Issue the display update for a previously Write1bppGram()'d area, with
    // the controller in 1bpp display mode. Panel-native coordinates.
    //
    // NOTE: this LATCHES 1bpp display mode on and returns without waiting for
    // the waveform (same fire-and-forget contract as UpdateArea()). The mode
    // bit is cleared lazily by the next 4bpp transfer, which waits on
    // CheckAFSR() first. Back-to-back 1bpp pushes therefore pay nothing.
    m5epd_err_t Update1bppArea(uint16_t px, uint16_t py, uint16_t pw,
                               uint16_t ph, m5epd_update_mode_t mode);

    // True while the controller is left in 1bpp display mode.
    bool Is1bppDisplayLatched(void) {
        return _display_1bpp_latched;
    }

    m5epd_err_t SetRotation(uint16_t rotate = IT8951_ROTATE_0);
    m5epd_err_t SetArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    m5epd_err_t UpdateFull(m5epd_update_mode_t mode);
    m5epd_err_t UpdateArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           m5epd_update_mode_t mode);

    uint16_t UpdateCount(void);
    uint8_t GetRotate(void) {
        return _rotate;
    };
    uint8_t GetDirection(void) {
        return _direction;
    };
    void ResetUpdateCount(void);
    m5epd_err_t CheckAFSR(void);
    SPIClass *GetSPI(void) {
        return _epd_spi;
    }
    void SetColorReverse(bool is_reverse);

    m5epd_err_t Active(void);
    m5epd_err_t StandBy(void);
    m5epd_err_t Sleep(void);

   private:
    void ResetDriver(void);
    m5epd_err_t GetSysInfo(void);
    m5epd_err_t WaitBusy(uint32_t timeout = 3000);
    m5epd_err_t WriteCommand(uint16_t cmd);
    m5epd_err_t WriteWord(uint16_t data);
    m5epd_err_t WriteReg(uint16_t addr, uint16_t data);
    m5epd_err_t ReadReg(uint16_t addr, uint16_t *data);
    void WriteGramData(uint16_t data);
    // MicroPatterns LOCAL ADDITION -- bulk gram transfer helpers.
    void WriteGramBulk(const uint8_t *data, uint32_t len, bool invert);
    void FillGramBulk(uint16_t word, uint32_t words);
    m5epd_err_t SetLoadArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t bpp, uint16_t rotate);
    m5epd_err_t LeaveDisplay1bpp(void);
    m5epd_err_t ReadWords(uint16_t *buf, uint32_t length);
    m5epd_err_t WriteArgs(uint16_t cmd, uint16_t *args, uint16_t length);
    m5epd_err_t SetTargetMemoryAddr(uint32_t tar_addr);
    void StartSPI(uint32_t freq);
    void StartSPI(void);
    void EndSPI(void);

   private:
    SPIClass *_epd_spi;
    int8_t _pin_cs, _pin_busy, _pin_sck, _pin_mosi, _pin_miso, _pin_rst;
    uint32_t _spi_freq;
    uint8_t _direction, _rotate;

    uint16_t _dev_memaddr_l, _dev_memaddr_h;
    uint32_t _tar_memaddr;
    uint16_t _endian_type, _pix_bpp;
    uint16_t _area_x, _area_y, _area_w, _area_h, _panel_w, _panel_h;
    uint16_t _fw_version[8], _lut_version[8];

    uint16_t _update_count;
    uint8_t _is_reverse;

    // MicroPatterns LOCAL ADDITION.
    bool _display_1bpp_latched;
    // 4-byte aligned ON PURPOSE: Arduino-ESP32's spiWriteNL() casts the buffer
    // to uint32_t* and reads it a long at a time. An unaligned buffer here is
    // an alignment fault (or a very slow exception-handler fixup), not a
    // wrong-pixels bug -- so it would present as a crash, not as corruption.
    uint8_t _xfer_buf[M5EPD_SPI_CHUNK_BYTES] __attribute__((aligned(4)));
};

#endif
