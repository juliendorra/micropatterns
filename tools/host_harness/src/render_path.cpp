#include "render_path.h"

#include <chrono>

#include "display_manager.h"
#include "display_list_renderer.h"
#include "host_display_manager.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"

namespace {

double msSince(const std::chrono::steady_clock::time_point& t0) {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0).count();
}

// The one path the C++ firmware ships today. Deliberately mirrors
// RenderController::renderScript() (M5Paper_MicroPatterns/src/render_controller.cpp)
// step for step, including the phase boundaries the device times with millis().
class DisplayListPath : public RenderPath {
public:
    // occlusion=false gives the same path with occlusion culling switched off.
    // Culling is supposed to be output-NEUTRAL -- it only skips items proven to
    // be completely covered by opaque items drawn later -- so the two must
    // produce byte-identical images. `compare-paths displaylist
    // displaylist-noocc` is what actually checks that; a golden image alone
    // cannot, because it only ever exercises one of the two.
    explicit DisplayListPath(bool occlusion = true, bool occupancyMap = true)
        : _occlusion(occlusion), _occupancyMap(occupancyMap),
          _name(!occupancyMap ? "displaylist-nomap"
                              : (occlusion ? "displaylist" : "displaylist-noocc")) {}

    const char* name() const override { return _name; }

    void run(const std::string& scriptText, int width, int height,
             const RenderSeed& seed, RenderResult& out) override {
        auto tTotal = std::chrono::steady_clock::now();

        hostSetCanvasSize(width, height);
        DisplayManager displayMgr;
        displayMgr.initializeEPD();

        // --- Phase 1: parse ------------------------------------------------
        MicroPatternsParser parser;
        auto t0 = std::chrono::steady_clock::now();
        bool parsed = parser.parse(String(scriptText.c_str()));
        out.timings.parseMs = msSince(t0);

        if (!parsed) {
            out.ok = false;
            out.error = "parse failed:";
            for (const String& e : parser.getErrors()) {
                out.error += "\n  ";
                out.error += e.c_str();
            }
            return;
        }

        // --- Phase 2: display list generation -------------------------------
        MicroPatternsRuntime runtime(displayMgr.getWidth(), displayMgr.getHeight(), parser.getAssets());
        runtime.setCommands(&parser.getCommands());
        runtime.setDeclaredVariables(&parser.getDeclaredVariables());
        runtime.setCounter(seed.counter);
        runtime.setTime(seed.hour, seed.minute, seed.second);

        t0 = std::chrono::steady_clock::now();
        runtime.generateDisplayList();
        out.timings.displayListMs = msSince(t0);

        // --- Phase 3: rasterization ------------------------------------------
        // DisplayListRenderer now takes the canvas directly (MPCanvas*) rather
        // than a DisplayManager& it stored but never read -- that decoupling is
        // what lets the Watchy firmware reuse this same renderer.
        DisplayListRenderer renderer(displayMgr.getCanvas(), parser.getAssets(),
                                     displayMgr.getWidth(), displayMgr.getHeight());
        renderer.setOcclusionEnabled(_occlusion);
        renderer.setOccupancyMapEnabled(_occupancyMap);
        t0 = std::chrono::steady_clock::now();
        renderer.render(runtime.getDisplayList());
        out.timings.rasterizeMs = msSince(t0);

        out.timings.totalMs = msSince(tTotal);

        out.counters.displayListItems = (int)runtime.getDisplayList().size();
        out.counters.totalItems = renderer.getTotalItems();
        out.counters.renderedItems = renderer.getRenderedItems();
        out.counters.culledOffScreen = renderer.getCulledOffScreen();
        out.counters.culledByOcclusion = renderer.getCulledByOcclusion();
        out.counters.overdrawSkippedPixels = renderer.getOverdrawSkippedPixels();

        // Read the canvas out as 8-bit gray. The canvas holds 4bpp values
        // 0..15; scale by 17 so 0 -> 0 (white on this device's convention is 0)
        // and 15 -> 255. Note the DSL treats 0 as white and 15 as black, so the
        // PNG is inverted relative to ink: we invert here so black ink reads as
        // black in the image.
        M5EPD_Canvas* canvas = displayMgr.getCanvas();
        out.image.width = width;
        out.image.height = height;
        out.image.pixels.assign((size_t)width * height, 0);
        long nonWhite = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                uint16_t v = canvas->readPixel(x, y);
                if (v != 0) nonWhite++;
                out.image.pixels[(size_t)y * width + x] = (uint8_t)(255 - (v * 17));
            }
        }
        out.counters.nonWhitePixels = nonWhite;
        out.ok = true;
    }

private:
    bool _occlusion;
    bool _occupancyMap;
    const char* _name;
};

} // namespace

std::unique_ptr<RenderPath> makeRenderPath(const std::string& name) {
    if (name == "displaylist") return std::unique_ptr<RenderPath>(new DisplayListPath(true));
    if (name == "displaylist-noocc") return std::unique_ptr<RenderPath>(new DisplayListPath(false));
    // What a Watchy renders when it cannot allocate the occupancy map.
    if (name == "displaylist-nomap") return std::unique_ptr<RenderPath>(new DisplayListPath(true, false));
    return nullptr;
}

std::vector<std::string> availableRenderPaths() {
    return {"displaylist", "displaylist-noocc", "displaylist-nomap"};
}
