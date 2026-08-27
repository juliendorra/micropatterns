#include "M5EPD_Driver.h"

#include <string.h>

// ===========================================================================
// VENDORED + LOCALLY MODIFIED. See lib/M5EPD/README-VENDORED.md.
// Upstream: m5stack/M5EPD 0.1.5.
// ===========================================================================

m5epd_err_t __epdret__;
#define CHECK(x)                  \
    __epdret__ = x;               \
    if (__epdret__ != M5EPD_OK) { \
        return __epdret__;        \
    }

M5EPD_Driver::M5EPD_Driver(int8_t spi_index) {
    if (spi_index > 0 && spi_index < 4) {
        _epd_spi = new SPIClass(spi_index);
    } else {
        _epd_spi = new SPIClass(VSPI);
    }
    _pin_cs   = -1;
    _pin_busy = -1;
    _pin_sck  = -1;
    _pin_mosi = -1;
    _pin_rst  = -1;

    _spi_freq = M5EPD_SPI_FREQ_HZ;

    _rotate    = IT8951_ROTATE_0;
    _direction = 1;

    _update_count = false;
    _is_reverse   = false;

    _display_1bpp_latched = false;
}

M5EPD_Driver::~M5EPD_Driver() {
    delete _epd_spi;
}

m5epd_err_t M5EPD_Driver::begin(int8_t sck, int8_t mosi, int8_t miso, int8_t cs,
                                int8_t busy, int8_t rst) {
    _epd_spi->begin(sck, miso, mosi, 4);
    _pin_cs   = cs;
    _pin_busy = busy;
    _pin_sck  = sck;
    _pin_mosi = mosi;
    _pin_miso = miso;
    _pin_rst  = rst;
    if (_pin_rst != -1) {
        pinMode(_pin_rst, OUTPUT);
        ResetDriver();
    }
    digitalWrite(_pin_cs, HIGH);
    pinMode(_pin_cs, OUTPUT);
    pinMode(_pin_busy, INPUT);

    StartSPI(M5EPD_SPI_FREQ_HZ);

    // CHECK(GetSysInfo());
    _tar_memaddr   = 0x001236E0;
    _dev_memaddr_l = 0x36E0;
    _dev_memaddr_h = 0x0012;
    CHECK(WriteCommand(IT8951_TCON_SYS_RUN));
    CHECK(WriteReg(IT8951_I80CPCR, 0x0001));  // enable pack write

    // set vcom to -2.30v
    CHECK(WriteCommand(0x0039));  // tcon vcom set command
    CHECK(WriteWord(0x0001));
    CHECK(WriteWord(2300));

    EndSPI();

    delay(1000);

    log_d("Init SUCCESS.");

    return M5EPD_OK;
}

/** @brief Invert display colors
 * @param is_reverse 1, reverse color; 0, default
 */
void M5EPD_Driver::SetColorReverse(bool is_reverse) {
    _is_reverse = is_reverse;
}

/** @brief Set panel rotation
 * @param rotate direction to rotate.
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::SetRotation(uint16_t rotate) {
    if (rotate < 4) {
        this->_rotate = rotate;
    } else if (rotate < 90) {
        this->_rotate = IT8951_ROTATE_0;
    } else if (rotate < 180) {
        this->_rotate = IT8951_ROTATE_90;
    } else if (rotate < 270) {
        this->_rotate = IT8951_ROTATE_180;
    } else {
        this->_rotate = IT8951_ROTATE_270;
    }

    if (_rotate == IT8951_ROTATE_0 || _rotate == IT8951_ROTATE_180) {
        _direction = 1;
    } else {
        _direction = 0;
    }
    return M5EPD_OK;
}

/** @brief Clear graphics buffer
 * @param init Screen initialization, If is 0, clear the buffer without
 * initializing
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::Clear(bool init) {
    CHECK(LeaveDisplay1bpp());  // LOCAL: back to 4bpp interpretation first

    _endian_type = IT8951_LDIMG_L_ENDIAN;
    _pix_bpp     = IT8951_4BPP;

    StartSPI();

    CHECK(SetTargetMemoryAddr(_tar_memaddr));
    if (_direction) {
        CHECK(SetArea(0, 0, M5EPD_PANEL_W, M5EPD_PANEL_H));
    } else {
        CHECK(SetArea(0, 0, M5EPD_PANEL_H, M5EPD_PANEL_W));
    }
    // LOCAL: was 129600 iterations of {CS low; write32(word); CS high}.
    FillGramBulk(_is_reverse ? 0x0000 : 0xFFFF,
                 ((uint32_t)M5EPD_PANEL_W * M5EPD_PANEL_H) >> 2);

    CHECK(WriteCommand(IT8951_TCON_LD_IMG_END));

    EndSPI();

    if (init) {
        CHECK(UpdateFull(UPDATE_MODE_INIT));
    }

    return M5EPD_OK;
}

/** @brief Write full (960 * 540) 4-bit (16 levels grayscale) image to panel.
 * @param gram pointer to image data.
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::WriteFullGram4bpp(const uint8_t *gram) {
    if (_direction) {
        return WritePartGram4bpp(0, 0, M5EPD_PANEL_W, M5EPD_PANEL_H, gram);
    } else {
        return WritePartGram4bpp(0, 0, M5EPD_PANEL_H, M5EPD_PANEL_W, gram);
    }
}

/** @brief Write the image at the specified location, Partial update
 * @param x Update X coordinate, >>> Must be a multiple of 4 <<<
 * @param y Update Y coordinate
 * @param w width of gram, >>> Must be a multiple of 4 <<<
 * @param h height of gram
 * @param gram 4bpp garm data
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::WritePartGram4bpp(uint16_t x, uint16_t y, uint16_t w,
                                            uint16_t h, const uint8_t *gram) {
    CHECK(LeaveDisplay1bpp());  // LOCAL: back to 4bpp interpretation first

    _endian_type = IT8951_LDIMG_B_ENDIAN;
    _pix_bpp     = IT8951_4BPP;

    // rounded up to be multiple of 4
    if (_direction) {
        x = (x + 3) & 0xFFFC;
    } else {
        x = (x + 3) & 0xFFFC;
        y = (y + 3) & 0xFFFC;
    }

    if (w & 0x03) {
        log_e("Gram width %d not a multiple of 4.", w);
        return M5EPD_NOTMULTIPLE4;
    }

    if (_direction) {
        if (x > M5EPD_PANEL_W || y > M5EPD_PANEL_H) {
            log_d("Pos (%d, %d) out of bounds.", x, y);
            return M5EPD_OUTOFBOUNDS;
        }
    } else {
        if (x > M5EPD_PANEL_H || y > M5EPD_PANEL_W) {
            log_d("Pos (%d, %d) out of bounds.", x, y);
            return M5EPD_OUTOFBOUNDS;
        }
    }

    StartSPI();

    CHECK(SetTargetMemoryAddr(_tar_memaddr));
    CHECK(SetArea(x, y, w, h));
    // LOCAL: was ((w*h)>>2) iterations of {CS low; write32(word); CS high},
    // i.e. 32 SPI bits and two GPIO toggles per 16 bits of payload. The wire
    // bytes produced there are exactly gram[] in order (MSBFIRST, B_ENDIAN),
    // optionally complemented, so one packed burst is bit-identical.
    // 0xFFFF - word == ~word for a full 16-bit word, hence the byte-wise
    // complement below.
    WriteGramBulk(gram, ((uint32_t)w * h) >> 1, !_is_reverse);
    CHECK(WriteCommand(IT8951_TCON_LD_IMG_END));

    EndSPI();

    return M5EPD_OK;
}

/** @brief Fill the color at the specified location, Partial update
 * @param x Update X coordinate, >>> Must be a multiple of 4 <<<
 * @param y Update Y coordinate
 * @param w width of gram, >>> Must be a multiple of 4 <<<
 * @param h height of gram
 * @param data 4bpp color
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::FillPartGram4bpp(uint16_t x, uint16_t y, uint16_t w,
                                           uint16_t h, uint16_t data) {
    CHECK(LeaveDisplay1bpp());  // LOCAL: back to 4bpp interpretation first

    _endian_type = IT8951_LDIMG_B_ENDIAN;
    _pix_bpp     = IT8951_4BPP;

    // rounded up to be multiple of 4
    // rounded up to be multiple of 4
    if (_direction) {
        x = (x + 3) & 0xFFFC;
    } else {
        x = (x + 3) & 0xFFFC;
        y = (y + 3) & 0xFFFC;
    }

    if (w & 0x03) {
        log_d("Gram width %d not a multiple of 4.", w);
        return M5EPD_NOTMULTIPLE4;
    }

    if (_direction) {
        if (x > M5EPD_PANEL_W || y > M5EPD_PANEL_H) {
            log_d("Pos (%d, %d) out of bounds.", x, y);
            return M5EPD_OUTOFBOUNDS;
        }
    } else {
        if (x > M5EPD_PANEL_H || y > M5EPD_PANEL_W) {
            log_d("Pos (%d, %d) out of bounds.", x, y);
            return M5EPD_OUTOFBOUNDS;
        }
    }

    // uint64_t length = (w / 2) * h;

    StartSPI();

    CHECK(SetTargetMemoryAddr(_tar_memaddr));
    CHECK(SetArea(x, y, w, h));
    FillGramBulk(data, ((uint32_t)w * h) >> 2);  // LOCAL: bulk, was per-word
    CHECK(WriteCommand(IT8951_TCON_LD_IMG_END));

    EndSPI();

    return M5EPD_OK;
}

/** @brief Full panel update
 * @param mode update mode
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::UpdateFull(m5epd_update_mode_t mode) {
    if (_direction) {
        CHECK(UpdateArea(0, 0, M5EPD_PANEL_W, M5EPD_PANEL_H, mode));
    } else {
        CHECK(UpdateArea(0, 0, M5EPD_PANEL_H, M5EPD_PANEL_W, mode));
    }

    return M5EPD_OK;
}

/** @brief Check if the device is busy
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::CheckAFSR(void) {
    uint32_t start_time = millis();
    while (1) {
        uint16_t infobuf[1];
        CHECK(WriteCommand(IT8951_TCON_REG_RD));
        CHECK(WriteWord(IT8951_LUTAFSR));
        CHECK(ReadWords(infobuf, 1));
        if (infobuf[0] == 0) {
            break;
        }

        if (millis() - start_time > 3000) {
            log_e("Device response timeout.");
            return M5EPD_BUSYTIMEOUT;
        }
    }
    return M5EPD_OK;
}

/** @brief Partial panel update
 * @param x Update X coordinate, >>> Must be a multiple of 4 <<<
 * @param y Update Y coordinate
 * @param w width of gram, >>> Must be a multiple of 4 <<<
 * @param h height of gram
 * @param mode update mode
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::UpdateArea(uint16_t x, uint16_t y, uint16_t w,
                                     uint16_t h, m5epd_update_mode_t mode) {
    if (mode == UPDATE_MODE_NONE) {
        return M5EPD_OTHERERR;
    }

    // rounded up to be multiple of 4
    if (_direction) {
        x = (x + 3) & 0xFFFC;
    } else {
        x = (x + 3) & 0xFFFC;
        y = (y + 3) & 0xFFFC;
    }

    CHECK(CheckAFSR());

    if (_direction) {
        if (x + w > M5EPD_PANEL_W) {
            w = M5EPD_PANEL_W - x;
        }
        if (y + h > M5EPD_PANEL_H) {
            h = M5EPD_PANEL_H - y;
        }
    } else {
        if (x + w > M5EPD_PANEL_H) {
            w = M5EPD_PANEL_H - x;
        }
        if (y + h > M5EPD_PANEL_W) {
            h = M5EPD_PANEL_W - y;
        }
    }

    uint16_t args[7];
    switch (_rotate) {
        case IT8951_ROTATE_0: {
            args[0] = x;
            args[1] = y;
            args[2] = w;
            args[3] = h;
            break;
        }
        case IT8951_ROTATE_90: {
            args[0] = y;
            args[1] = M5EPD_PANEL_H - w - x;
            args[2] = h;
            args[3] = w;
            break;
        }
        case IT8951_ROTATE_180: {
            args[0] = M5EPD_PANEL_W - w - x;
            args[1] = M5EPD_PANEL_H - h - y;
            args[2] = w;
            args[3] = h;
            break;
        }
        case IT8951_ROTATE_270: {
            args[0] = M5EPD_PANEL_W - h - y;
            args[1] = x;
            args[2] = h;
            args[3] = w;
            break;
        }
    }

    args[4] = mode;
    args[5] = _dev_memaddr_l;
    args[6] = _dev_memaddr_h;

    StartSPI();
    CHECK(WriteArgs(IT8951_I80_CMD_DPY_BUF_AREA, args, 7));
    EndSPI();

    _update_count++;

    return M5EPD_OK;
}

/** @brief  Set write area
 * @param x Update X coordinate, >>> Must be a multiple of 4 <<<
 * @param y Update Y coordinate
 * @param w width of gram, >>> Must be a multiple of 4 <<<
 * @param h height of gram
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::SetArea(uint16_t x, uint16_t y, uint16_t w,
                                  uint16_t h) {
    // LOCAL: body moved to SetLoadArea() so the 1bpp path can override the
    // bpp/rotate fields without disturbing the members. Behaviour unchanged.
    return SetLoadArea(x, y, w, h, _pix_bpp, _rotate);
}

/** @brief LOCAL ADDITION. Set write area with explicit bpp and rotate.
 */
m5epd_err_t M5EPD_Driver::SetLoadArea(uint16_t x, uint16_t y, uint16_t w,
                                      uint16_t h, uint16_t bpp,
                                      uint16_t rotate) {
    uint16_t args[5];
    args[0] = (_endian_type << 8 | bpp << 4 | rotate);
    args[1] = x;
    args[2] = y;
    args[3] = w;
    args[4] = h;
    CHECK(WriteArgs(IT8951_TCON_LD_IMG_AREA, args, 5));

    return M5EPD_OK;
}

/** @brief  Write image data to the set address
 * @param data pointer to 4-bpp gram data
 * @retval m5epd_err_t
 */
void M5EPD_Driver::WriteGramData(uint16_t data) {
    digitalWrite(_pin_cs, 0);
    _epd_spi->write32(data);
    // _epd_spi->write16(0x0000);
    // _epd_spi->write16(data);
    digitalWrite(_pin_cs, 1);
}

m5epd_err_t M5EPD_Driver::SetTargetMemoryAddr(uint32_t tar_addr) {
    uint16_t h = (uint16_t)((tar_addr >> 16) & 0x0000FFFF);
    uint16_t l = (uint16_t)(tar_addr & 0x0000FFFF);

    CHECK(WriteReg(IT8951_LISAR + 2, h));
    CHECK(WriteReg(IT8951_LISAR, l));

    return M5EPD_OK;
}

/** @brief Set power mode to Active
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::Active(void) {
    StartSPI();
    CHECK(WriteCommand(IT8951_TCON_SYS_RUN));
    EndSPI();

    return M5EPD_OK;
}

/** @brief Set power mode to StandBy
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::StandBy(void) {
    StartSPI();
    CHECK(WriteCommand(IT8951_TCON_STANDBY));
    EndSPI();
    CHECK(WaitBusy());

    return M5EPD_OK;
}

/** @brief Set power mode to Sleep
 * @retval m5epd_err_t
 */
m5epd_err_t M5EPD_Driver::Sleep(void) {
    StartSPI();
    CHECK(WriteCommand(IT8951_TCON_SLEEP));
    EndSPI();
    CHECK(WaitBusy());

    return M5EPD_OK;
}

m5epd_err_t M5EPD_Driver::WriteReg(uint16_t addr, uint16_t data) {
    CHECK(WriteCommand(0x0011));  // tcon write reg command
    CHECK(WriteWord(addr));
    CHECK(WriteWord(data));
    return M5EPD_OK;
}

/** @brief LOCAL ADDITION. Read one IT8951 register.
 *  Caller must already be inside a StartSPI()/EndSPI() pair, matching the
 *  convention of WriteReg() above.
 */
m5epd_err_t M5EPD_Driver::ReadReg(uint16_t addr, uint16_t *data) {
    CHECK(WriteCommand(IT8951_TCON_REG_RD));
    CHECK(WriteWord(addr));
    CHECK(ReadWords(data, 1));
    return M5EPD_OK;
}

m5epd_err_t M5EPD_Driver::GetSysInfo(void) {
    uint16_t infobuf[20];
    CHECK(WriteCommand(IT8951_I80_CMD_GET_DEV_INFO));
    CHECK(ReadWords(infobuf, 20));
    _dev_memaddr_l = infobuf[2];
    _dev_memaddr_h = infobuf[3];
    _tar_memaddr   = (_dev_memaddr_h << 16) | _dev_memaddr_l;
    log_d("memory addr = %04X%04X", _dev_memaddr_h, _dev_memaddr_l);
    return M5EPD_OK;
}

void M5EPD_Driver::ResetDriver(void) {
    digitalWrite(_pin_rst, 1);
    digitalWrite(_pin_rst, 0);
    delay(20);
    digitalWrite(_pin_rst, 1);
    delay(100);
}

void M5EPD_Driver::StartSPI(uint32_t freq) {
    _epd_spi->beginTransaction(SPISettings(freq, MSBFIRST, SPI_MODE0));
}

void M5EPD_Driver::StartSPI(void) {
    _epd_spi->beginTransaction(SPISettings(_spi_freq, MSBFIRST, SPI_MODE0));
}

void M5EPD_Driver::EndSPI(void) {
    _epd_spi->endTransaction();
}

m5epd_err_t M5EPD_Driver::WaitBusy(uint32_t timeout) {
    uint32_t start_time = millis();
    while (1) {
        if (digitalRead(_pin_busy) == 1) {
            return M5EPD_OK;
        }

        if (millis() - start_time > timeout) {
            log_e("Device response timeout.");
            return M5EPD_BUSYTIMEOUT;
        }
    }
}

m5epd_err_t M5EPD_Driver::WriteCommand(uint16_t cmd) {
    CHECK(WaitBusy());
    digitalWrite(_pin_cs, 0);
    _epd_spi->write16(0x6000);
    CHECK(WaitBusy());
    _epd_spi->write16(cmd);
    digitalWrite(_pin_cs, 1);

    return M5EPD_OK;
}

m5epd_err_t M5EPD_Driver::WriteWord(uint16_t data) {
    CHECK(WaitBusy());
    digitalWrite(_pin_cs, 0);
    _epd_spi->write16(0x0000);
    CHECK(WaitBusy());
    _epd_spi->write16(data);
    digitalWrite(_pin_cs, 1);

    return M5EPD_OK;
}

m5epd_err_t M5EPD_Driver::ReadWords(uint16_t *buf, uint32_t length) {
    // uint16_t dummy;
    CHECK(WaitBusy());
    digitalWrite(_pin_cs, 0);
    _epd_spi->write16(0x1000);
    CHECK(WaitBusy());

    // dummy
    _epd_spi->transfer16(0);
    CHECK(WaitBusy());

    for (size_t i = 0; i < length; i++) {
        buf[i] = _epd_spi->transfer16(0);
    }

    digitalWrite(_pin_cs, 1);
    return M5EPD_OK;
}

m5epd_err_t M5EPD_Driver::WriteArgs(uint16_t cmd, uint16_t *args,
                                    uint16_t length) {
    CHECK(WriteCommand(cmd));
    for (uint16_t i = 0; i < length; i++) {
        CHECK(WriteWord(args[i]));
    }
    return M5EPD_OK;
}

uint16_t M5EPD_Driver::UpdateCount(void) {
    return _update_count;
}

void M5EPD_Driver::ResetUpdateCount(void) {
    _update_count = 0;
}

// ===========================================================================
// MicroPatterns LOCAL ADDITIONS
// ===========================================================================

/** @brief Ship `len` bytes of gram payload in ONE CS assertion.
 *
 * UPSTREAM shipped one 16-bit payload word per CS assertion:
 *     digitalWrite(CS,0); spi->write32(word); digitalWrite(CS,1);
 * write32() of a 16-bit value emits 0x0000 -- the IT8951 "write data"
 * preamble -- followed by the 16 payload bits. So every 16 bits of image cost
 * 32 bits on the wire plus two GPIO toggles, and a full 540x960 4bpp frame
 * cost 129,600 of those.
 *
 * IT8951 pack-write mode (IT8951_I80CPCR bit 0, already enabled by begin())
 * accepts ONE preamble per CS assertion followed by an arbitrary number of
 * payload words. That is exactly what Waveshare's
 * EPD_IT8951_HostAreaPackedPixelWrite() does, and it halves the wire traffic
 * on top of removing the GPIO churn.
 *
 * @param invert byte-wise complement on the way out. Upstream computed
 *        `0xFFFF - word`, which for a full 16-bit word is `~word`, which is
 *        the byte-wise complement -- so this is bit-identical.
 */
void M5EPD_Driver::WriteGramBulk(const uint8_t *data, uint32_t len,
                                 bool invert) {
    if (len == 0) {
        return;
    }

    digitalWrite(_pin_cs, 0);
    _epd_spi->write16(0x0000);  // pack-write preamble, ONCE for the burst

    if (!invert) {
        _epd_spi->writeBytes(data, len);
    } else {
        uint32_t done = 0;
        while (done < len) {
            uint32_t n = len - done;
            if (n > M5EPD_SPI_CHUNK_BYTES) {
                n = M5EPD_SPI_CHUNK_BYTES;
            }
            const uint8_t *src = data + done;
            const uint32_t n32 = n >> 2;
            for (uint32_t i = 0; i < n32; i++) {
                uint32_t v;
                memcpy(&v, src + (i << 2), 4);
                v = ~v;
                memcpy(_xfer_buf + (i << 2), &v, 4);
            }
            for (uint32_t i = n32 << 2; i < n; i++) {
                _xfer_buf[i] = (uint8_t)(~src[i]);
            }
            _epd_spi->writeBytes(_xfer_buf, n);
            done += n;
        }
    }

    digitalWrite(_pin_cs, 1);
}

/** @brief Ship `words` copies of a constant 16-bit payload word in one burst.
 */
void M5EPD_Driver::FillGramBulk(uint16_t word, uint32_t words) {
    if (words == 0) {
        return;
    }

    const uint32_t chunk_words = M5EPD_SPI_CHUNK_BYTES >> 1;
    for (uint32_t i = 0; i < chunk_words; i++) {
        _xfer_buf[(i << 1)]     = (uint8_t)(word >> 8);
        _xfer_buf[(i << 1) + 1] = (uint8_t)(word & 0xFF);
    }

    digitalWrite(_pin_cs, 0);
    _epd_spi->write16(0x0000);  // pack-write preamble, ONCE for the burst
    while (words) {
        uint32_t n = (words > chunk_words) ? chunk_words : words;
        _epd_spi->writeBytes(_xfer_buf, n << 1);
        words -= n;
    }
    digitalWrite(_pin_cs, 1);
}

/** @brief Leave 1bpp display mode if we latched it.
 *
 * Update1bppArea() deliberately returns without waiting for the waveform, so
 * the UP1SR bit is still set while the panel settles. Clearing it mid-update
 * would make the display engine re-read the same DRAM as 4bpp and garble the
 * frame, so we wait for the LUT engines to go idle first. This is called at
 * the top of every 4bpp transfer, which is where the wait belongs: upstream
 * already paid a CheckAFSR() there via UpdateArea().
 *
 * Back-to-back 1bpp pushes never call this, so they pay nothing.
 */
m5epd_err_t M5EPD_Driver::LeaveDisplay1bpp(void) {
    if (!_display_1bpp_latched) {
        return M5EPD_OK;
    }

    CHECK(CheckAFSR());

    uint16_t up1sr = 0;
    StartSPI();
    CHECK(ReadReg(IT8951_UP1SR + 2, &up1sr));
    CHECK(WriteReg(IT8951_UP1SR + 2, (uint16_t)(up1sr & ~(1 << 2))));
    EndSPI();

    _display_1bpp_latched = false;
    return M5EPD_OK;
}

/** @brief Load a packed 1bpp image into controller DRAM.
 *
 * The trick, straight out of Waveshare's EPD_IT8951.c
 * (EPD_IT8951_1bp_Refresh): declare the load as IT8951_8BPP but divide X and
 * W by 8. The controller then stores each byte we send as one "8bpp pixel",
 * which is really 8 packed 1bpp pixels; the display engine unpacks them when
 * the UP1SR 1bpp bit is set. Their own comment is "Use 8bpp to set 1bpp".
 *
 * px/py/pw/ph are PANEL-NATIVE (rotate-0) coordinates. See the header.
 */
m5epd_err_t M5EPD_Driver::Write1bppGram(uint16_t px, uint16_t py, uint16_t pw,
                                        uint16_t ph, const uint8_t *packed) {
    if ((px % M5EPD_1BPP_ALIGN) || (pw % M5EPD_1BPP_ALIGN)) {
        log_e("1bpp area x=%u w=%u not %u-aligned.", px, pw, M5EPD_1BPP_ALIGN);
        return M5EPD_NOTMULTIPLE4;
    }
    if ((uint32_t)px + pw > M5EPD_PANEL_W ||
        (uint32_t)py + ph > M5EPD_PANEL_H) {
        log_e("1bpp area (%u,%u,%u,%u) out of bounds.", px, py, pw, ph);
        return M5EPD_OUTOFBOUNDS;
    }

    _endian_type = M5EPD_1BPP_ENDIAN;
    _pix_bpp     = IT8951_8BPP;

    StartSPI();
    CHECK(SetTargetMemoryAddr(_tar_memaddr));
    // Rotation is forced to 0: the IT8951 applies rotation during the LOAD,
    // which would rotate whole packed bytes instead of pixels. The caller has
    // already transposed.
    CHECK(SetLoadArea(px >> 3, py, pw >> 3, ph, IT8951_8BPP, IT8951_ROTATE_0));
    WriteGramBulk(packed, ((uint32_t)pw >> 3) * ph, false);
    CHECK(WriteCommand(IT8951_TCON_LD_IMG_END));
    EndSPI();

    return M5EPD_OK;
}

/** @brief Display a previously Write1bppGram()'d area in 1bpp display mode.
 *
 * Sequence per Waveshare EPD_IT8951_Display_1bp():
 *   1. UP1SR+2 |= (1<<2)          -- enable 1bpp display mode
 *   2. BGVR = (front<<8) | back   -- the two greys the 0/1 bits map to
 *   3. DPY_BUF_AREA               -- fire the waveform
 *   4. (later) clear the UP1SR bit -- done lazily, see LeaveDisplay1bpp()
 *
 * px/py/pw/ph are PANEL-NATIVE (rotate-0) coordinates and are passed through
 * unrotated -- unlike UpdateArea(), which transforms canvas coordinates.
 */
m5epd_err_t M5EPD_Driver::Update1bppArea(uint16_t px, uint16_t py, uint16_t pw,
                                         uint16_t ph,
                                         m5epd_update_mode_t mode) {
    if (mode == UPDATE_MODE_NONE) {
        return M5EPD_OTHERERR;
    }
    if ((px % M5EPD_1BPP_ALIGN) || (pw % M5EPD_1BPP_ALIGN)) {
        return M5EPD_NOTMULTIPLE4;
    }

    if ((uint32_t)px + pw > M5EPD_PANEL_W) {
        pw = M5EPD_PANEL_W - px;
    }
    if ((uint32_t)py + ph > M5EPD_PANEL_H) {
        ph = M5EPD_PANEL_H - py;
    }

    // Retuning UP1SR while a waveform runs would change how the running
    // update interprets DRAM, so wait for the previous one. UpdateArea() does
    // the same thing at the same point.
    CHECK(CheckAFSR());

    uint16_t up1sr = 0;
    StartSPI();
    CHECK(ReadReg(IT8951_UP1SR + 2, &up1sr));
    CHECK(WriteReg(IT8951_UP1SR + 2, (uint16_t)(up1sr | (1 << 2))));
    CHECK(WriteReg(IT8951_BGVR,
                   (uint16_t)((M5EPD_1BPP_FRONT_GREY << 8) |
                              M5EPD_1BPP_BACK_GREY)));

    uint16_t args[7];
    args[0] = px;
    args[1] = py;
    args[2] = pw;
    args[3] = ph;
    args[4] = mode;
    args[5] = _dev_memaddr_l;
    args[6] = _dev_memaddr_h;
    CHECK(WriteArgs(IT8951_I80_CMD_DPY_BUF_AREA, args, 7));
    EndSPI();

    _display_1bpp_latched = true;
    _update_count++;

    return M5EPD_OK;
}
