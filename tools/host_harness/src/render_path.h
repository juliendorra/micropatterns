// Render-path abstraction for the host harness.
//
// This project has a demonstrated practice of building competing renderer
// implementations, proving they produce IDENTICAL output, and only then
// benchmarking to pick a winner (see the JS emulator's interpreter / compiler /
// display-list paths, and commit d427b02 "All path gives same result", followed
// by 68d843e "Default to display list").
//
// The C++ firmware currently ships exactly ONE path: parse -> display list ->
// rasterize. This header is the seam that lets a second C++ path be dropped in
// and diffed against the first, byte for byte, using the same corpus and the
// same phase-timing instrumentation. To add one: subclass RenderPath, fill in
// the same PhaseTimings/RenderCounters fields, and register it in
// render_path.cpp::makeRenderPath(). `mpharness compare-paths` will then
// byte-compare it against the display-list path with no further work.
#ifndef HOST_HARNESS_RENDER_PATH_H
#define HOST_HARNESS_RENDER_PATH_H

#include <memory>
#include <string>
#include <vector>

#include "image_io.h"

struct RenderSeed {
    int counter = 0;
    int hour = 12;
    int minute = 34;
    int second = 56;
};

struct PhaseTimings {
    double parseMs = 0.0;        // MicroPatternsParser::parse
    double displayListMs = 0.0;  // MicroPatternsRuntime::generateDisplayList
    double rasterizeMs = 0.0;    // DisplayListRenderer::render
    double totalMs = 0.0;        // sum of the three, plus object setup
};

struct RenderCounters {
    int displayListItems = 0;
    int totalItems = 0;
    int renderedItems = 0;
    int culledOffScreen = 0;
    int culledByOcclusion = 0;
    unsigned int overdrawSkippedPixels = 0; // pixels skipped by the occupancy map
    // Honest label: this is "pixels whose final value differs from white",
    // derived by scanning the canvas. It is NOT a count of rawPixel() calls --
    // the drawing layer does not track that. Use it as a coverage proxy only.
    long nonWhitePixels = 0;
};

struct RenderResult {
    bool ok = false;
    std::string error;
    GrayImage image;
    PhaseTimings timings;
    RenderCounters counters;
};

class RenderPath {
public:
    virtual ~RenderPath() {}
    virtual const char* name() const = 0;
    virtual void run(const std::string& scriptText, int width, int height,
                     const RenderSeed& seed, RenderResult& out) = 0;
};

// Returns nullptr for an unknown name. Currently: "displaylist".
std::unique_ptr<RenderPath> makeRenderPath(const std::string& name);
std::vector<std::string> availableRenderPaths();

#if MP_DEVICE_CONSTRAINTS
// Drops persistent device-owned render objects after a simulated fatal OOM.
// The next render starts from that profile's reboot/task-start boundary.
void resetDeviceRenderSessionAfterFailure();
#endif

#endif // HOST_HARNESS_RENDER_PATH_H
