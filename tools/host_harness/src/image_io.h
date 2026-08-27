// Dependency-free 8-bit grayscale image I/O for the host harness.
//   - PNG writer  : self-contained (stored/uncompressed deflate + crc32/adler32).
//   - PGM writer  : binary P5.
//   - PGM reader  : used for byte-comparing against stored goldens.
// Goldens are stored as PGM so `verify` can diff raw pixels without needing an
// inflate implementation.
#ifndef HOST_HARNESS_IMAGE_IO_H
#define HOST_HARNESS_IMAGE_IO_H

#include <cstdint>
#include <string>
#include <vector>

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // width*height, 8-bit gray
};

bool writePNG(const std::string& path, const GrayImage& img, std::string& err);
bool writePGM(const std::string& path, const GrayImage& img, std::string& err);
bool readPGM(const std::string& path, GrayImage& img, std::string& err);

// Writes by extension (.png -> PNG, anything else -> PGM).
bool writeImage(const std::string& path, const GrayImage& img, std::string& err);

#endif // HOST_HARNESS_IMAGE_IO_H
