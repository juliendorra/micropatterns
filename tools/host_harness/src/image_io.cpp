#include "image_io.h"

#include <cstdio>
#include <cstring>

namespace {

uint32_t crcTable[256];
bool crcTableReady = false;

void makeCrcTable() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[n] = c;
    }
    crcTableReady = true;
}

uint32_t crc32buf(const uint8_t* buf, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    if (!crcTableReady) makeCrcTable();
    for (size_t i = 0; i < len; i++) crc = crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t adler32buf(const uint8_t* buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void pushChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    std::vector<uint8_t> typed(type, type + 4);
    out.insert(out.end(), typed.begin(), typed.end());
    out.insert(out.end(), data.begin(), data.end());
    std::vector<uint8_t> crcInput = typed;
    crcInput.insert(crcInput.end(), data.begin(), data.end());
    put32(out, crc32buf(crcInput.data(), crcInput.size()) ^ 0xFFFFFFFFu);
}

// Wraps raw bytes in a zlib stream made only of stored (uncompressed) deflate
// blocks. Larger files than real deflate, but zero dependencies and every PNG
// viewer reads it.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> z;
    z.push_back(0x78); // CMF: deflate, 32K window
    z.push_back(0x01); // FLG: no dict, fastest
    size_t pos = 0;
    const size_t kBlock = 65535;
    while (pos < raw.size() || raw.empty()) {
        size_t n = raw.size() - pos;
        if (n > kBlock) n = kBlock;
        bool last = (pos + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    put32(z, adler32buf(raw.data(), raw.size()));
    return z;
}

bool writeAll(const std::string& path, const std::vector<uint8_t>& bytes, std::string& err) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open for write: " + path; return false; }
    size_t n = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    if (n != bytes.size()) { err = "short write: " + path; return false; }
    return true;
}

} // namespace

bool writePNG(const std::string& path, const GrayImage& img, std::string& err) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() != (size_t)img.width * img.height) {
        err = "writePNG: bad image dimensions";
        return false;
    }
    // Filter type 0 (None) prefixed to each scanline.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)img.height * (img.width + 1));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);
        const uint8_t* row = img.pixels.data() + (size_t)y * img.width;
        raw.insert(raw.end(), row, row + img.width);
    }

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)img.width);
    put32(ihdr, (uint32_t)img.height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(0); // color type: grayscale
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    pushChunk(out, "IHDR", ihdr);
    pushChunk(out, "IDAT", zlibStored(raw));
    pushChunk(out, "IEND", {});

    return writeAll(path, out, err);
}

bool writePGM(const std::string& path, const GrayImage& img, std::string& err) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() != (size_t)img.width * img.height) {
        err = "writePGM: bad image dimensions";
        return false;
    }
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "P5\n%d %d\n255\n", img.width, img.height);
    std::vector<uint8_t> out(hdr, hdr + n);
    out.insert(out.end(), img.pixels.begin(), img.pixels.end());
    return writeAll(path, out, err);
}

bool readPGM(const std::string& path, GrayImage& img, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open: " + path; return false; }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);

    size_t p = 0;
    auto skipWs = [&]() {
        while (p < data.size()) {
            if (data[p] == '#') { while (p < data.size() && data[p] != '\n') p++; }
            else if (isspace(data[p])) p++;
            else break;
        }
    };
    auto readInt = [&](int& outv) -> bool {
        skipWs();
        if (p >= data.size() || !isdigit(data[p])) return false;
        int v = 0;
        while (p < data.size() && isdigit(data[p])) { v = v * 10 + (data[p] - '0'); p++; }
        outv = v;
        return true;
    };

    if (data.size() < 2 || data[0] != 'P' || data[1] != '5') { err = "not a binary PGM: " + path; return false; }
    p = 2;
    int w = 0, h = 0, maxv = 0;
    if (!readInt(w) || !readInt(h) || !readInt(maxv)) { err = "bad PGM header: " + path; return false; }
    if (maxv != 255) { err = "unsupported PGM maxval: " + path; return false; }
    p++; // exactly one whitespace byte after maxval
    if (data.size() - p < (size_t)w * h) { err = "truncated PGM: " + path; return false; }
    img.width = w;
    img.height = h;
    img.pixels.assign(data.begin() + p, data.begin() + p + (size_t)w * h);
    return true;
}

bool writeImage(const std::string& path, const GrayImage& img, std::string& err) {
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0)
        return writePNG(path, img, err);
    return writePGM(path, img, err);
}
