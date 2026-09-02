#include "render_path.h"

#include <chrono>
#include <new>

#include "display_manager.h"
#include "display_list_renderer.h"
#include "host_display_manager.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "mp_program.h"

#if MP_DEVICE_CONSTRAINTS
#include "device_allocator.h"
#endif

namespace {

double msSince(const std::chrono::steady_clock::time_point& t0) {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0).count();
}

#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
// RenderController lives for the lifetime of M5Paper's RenderTask. Its parser
// is a member and its latest runtime/renderer remain allocated between jobs.
// Raw pointers are intentional: a simulated reboot discards the entire arena.
struct M5RenderSession {
    bool initialized = false;
    MpDeviceState state = MpDeviceState::RadiosOff;
    MicroPatternsParser* parser = nullptr;
    MicroPatternsRuntime* runtime = nullptr;
    DisplayListRenderer* renderer = nullptr;
};

M5RenderSession g_m5Session;
alignas(MicroPatternsParser)
unsigned char g_m5ParserStorage[sizeof(MicroPatternsParser)];

void destroyM5Objects()
{
    if (!g_m5Session.initialized) return;
    mpDeviceSetAllocationActive(true);
    delete g_m5Session.renderer;
    delete g_m5Session.runtime;
    g_m5Session.parser->~MicroPatternsParser();
    mpDeviceSetAllocationActive(false);
    g_m5Session.renderer = nullptr;
    g_m5Session.runtime = nullptr;
    g_m5Session.parser = nullptr;
    g_m5Session.initialized = false;
}

bool ensureM5Session()
{
    const MpDeviceState requested = mpDeviceRequestedState();
    if (g_m5Session.initialized && g_m5Session.state == requested) return true;
    destroyM5Objects();
    if (!mpDeviceAllocatorReset(requested)) return false;
    mpDeviceSetAllocationActive(true);
    mpDeviceSetPhase(MpAllocationPhase::Idle);
    // RenderController (including its parser member) lives on the task stack;
    // placement construction keeps the object itself in support memory while
    // its STL/String payloads still use the active device allocator.
    g_m5Session.parser = new (g_m5ParserStorage) MicroPatternsParser();
    mpDeviceSetAllocationActive(false);
    g_m5Session.state = requested;
    g_m5Session.initialized = true;
    return true;
}
#endif

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
    // roundtrip=true serializes the compiled program to bytes and loads it
    // back before running -- the exact path a device takes when it renders a
    // program stored at sync time. It must be byte-identical to the direct
    // path; `compare-paths displaylist compiled` is what checks that.
    explicit DisplayListPath(bool occlusion = true, bool occupancyMap = true, bool roundtrip = false)
        : _occlusion(occlusion), _occupancyMap(occupancyMap), _roundtrip(roundtrip),
          _name(roundtrip ? "compiled"
                          : (!occupancyMap ? "displaylist-nomap"
                                           : (occlusion ? "displaylist" : "displaylist-noocc"))) {}

    const char* name() const override { return _name; }

    void run(const std::string& scriptText, int width, int height,
             const RenderSeed& seed, RenderResult& out) override {
        auto tTotal = std::chrono::steady_clock::now();

        hostSetCanvasSize(width, height);
        DisplayManager displayMgr;
        displayMgr.initializeEPD();

#if MP_DEVICE_CONSTRAINTS
#if defined(MP_DEVICE_M5PAPER)
        if (!ensureM5Session()) {
#else
        if (!mpDeviceAllocatorReset(mpDeviceRequestedState())) {
#endif
            out.ok = false;
            out.error = "device heap profile could not be initialized";
            return;
        }
        mpDeviceBeginRenderTelemetry();
        struct AllocationScope {
            AllocationScope() { mpDeviceSetAllocationActive(true); }
            ~AllocationScope() { mpDeviceSetAllocationActive(false); }
        } allocationScope;
        mpDeviceSetPhase(MpAllocationPhase::Source);
#endif

        // --- Phase 1: parse ------------------------------------------------
#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        MicroPatternsParser& parser = *g_m5Session.parser;
#else
        MicroPatternsParser parser;
#endif
        String source(scriptText.c_str());
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::Parse);
#endif
        auto t0 = std::chrono::steady_clock::now();
        bool parsed = parser.parse(source);
        out.timings.parseMs = msSince(t0);

        if (!parsed) {
#if MP_DEVICE_CONSTRAINTS
            // Error transport belongs to the browser harness, not the device
            // heap. Parser-owned error Strings remain simulated allocations.
            mpDeviceSetAllocationActive(false);
#endif
            out.ok = false;
            out.error = "parse failed:";
            for (const String& e : parser.getErrors()) {
                out.error += "\n  ";
                out.error += e.c_str();
            }
            return;
        }

        // The program the runtime executes. Either the parser's own, or -- on
        // the "compiled" path -- one rebuilt from its serialized bytes, as the
        // device does from the file it wrote at sync time.
        MpProgram loaded;
        const MpProgram* programToRun = &parser.getProgram();
        out.counters.programBytes = parser.getProgram().byteSize();
        if (_roundtrip) {
            // Same lifetimes as ScriptManager on the device: the parser's
            // program is released once its bytes exist, and the bytes are
            // released once the program has been rebuilt from them, so the
            // peak here is what the device's peak is.
            std::vector<uint8_t> bytes;
            mp_program_fingerprint(parser.program(), (const uint8_t*)scriptText.data(), scriptText.size());
#if MP_DEVICE_CONSTRAINTS
            // The serialized bytes stand in for the file on flash: the device
            // writes them out at sync and later streams them back in without
            // ever holding the whole file in RAM (ScriptManager's FileReader).
            // So they live in host memory here, not in the simulated heap --
            // only the compile (parser + program) and the load (program
            // rebuilt from the stream) are device allocations.
            mpDeviceSetAllocationActive(false);
#endif
            const bool serialized = mp_program_serialize(parser.getProgram(), bytes);
#if MP_DEVICE_CONSTRAINTS
            mpDeviceSetAllocationActive(true);
#endif
            if (!serialized) {
                out.ok = false; out.error = "program serialization failed"; return;
            }
            out.counters.programFileBytes = bytes.size();
            parser.reset();
            String why;
            const bool ok = mp_program_deserialize(bytes.data(), bytes.size(), loaded, &why);
#if MP_DEVICE_CONSTRAINTS
            mpDeviceSetAllocationActive(false);
#endif
            std::vector<uint8_t>().swap(bytes);
#if MP_DEVICE_CONSTRAINTS
            mpDeviceSetAllocationActive(true);
#endif
            if (!ok) {
                out.ok = false; out.error = std::string("program deserialization failed: ") + why.c_str(); return;
            }
            programToRun = &loaded;
        }

#if defined(MP_DEVICE_WATCHY)
        // The Watchy explicitly releases source before building the list.
        source = String();
#endif

        // --- Phase 2: display list generation -------------------------------
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::DisplayList);
#endif
#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        // RenderController deletes the previous runtime only after the new
        // script has parsed, so its retained capacity contributes to parsing.
        delete g_m5Session.runtime;
        g_m5Session.runtime = nullptr;
        g_m5Session.runtime = new MicroPatternsRuntime(
            displayMgr.getWidth(), displayMgr.getHeight(), *programToRun);
        MicroPatternsRuntime& runtime = *g_m5Session.runtime;
#else
        MicroPatternsRuntime runtime(displayMgr.getWidth(), displayMgr.getHeight(), *programToRun);
#endif
        runtime.setCounter(seed.counter);
        runtime.setTime(seed.hour, seed.minute, seed.second);

        t0 = std::chrono::steady_clock::now();
        runtime.generateDisplayList();
        out.timings.displayListMs = msSince(t0);

        // --- Phase 3: rasterization ------------------------------------------
        // DisplayListRenderer now takes the canvas directly (MPCanvas*) rather
        // than a DisplayManager& it stored but never read -- that decoupling is
        // what lets the Watchy firmware reuse this same renderer.
#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        delete g_m5Session.renderer;
        g_m5Session.renderer = nullptr;
        g_m5Session.renderer = new DisplayListRenderer(
            displayMgr.getCanvas(), displayMgr.getWidth(), displayMgr.getHeight());
        DisplayListRenderer& renderer = *g_m5Session.renderer;
#else
        DisplayListRenderer renderer(displayMgr.getCanvas(),
                                     displayMgr.getWidth(), displayMgr.getHeight());
#endif
        renderer.setOcclusionEnabled(_occlusion);
        renderer.setOccupancyMapEnabled(_occupancyMap);
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::Rasterize);
#endif
        t0 = std::chrono::steady_clock::now();
        renderer.render(runtime.getDisplayList());
        out.timings.rasterizeMs = msSince(t0);

        out.timings.totalMs = msSince(tTotal);

        out.counters.displayListItems = (int)runtime.getDisplayList().size();
        out.counters.displayListBytes = runtime.getDisplayList().size() * sizeof(DisplayListItem);
        out.counters.totalItems = renderer.getTotalItems();
        out.counters.renderedItems = renderer.getRenderedItems();
        out.counters.culledOffScreen = renderer.getCulledOffScreen();
        out.counters.culledByOcclusion = renderer.getCulledByOcclusion();
        out.counters.overdrawSkippedPixels = renderer.getOverdrawSkippedPixels();
        out.counters.occupancyMapUsed = renderer.usedOccupancyMapLastRender();

#if MP_DEVICE_CONSTRAINTS
        // The gray browser output is not a device framebuffer. Allocate it from
        // support memory while the device-owned objects remain alive.
        mpDeviceSetAllocationActive(false);
        mpDeviceSetPhase(MpAllocationPhase::Output);
#endif

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

    bool compile(const std::string& scriptText, std::vector<uint8_t>& outBytes,
                 RenderResult& out) override {
        outBytes.clear();
#if MP_DEVICE_CONSTRAINTS
#if defined(MP_DEVICE_M5PAPER)
        if (!ensureM5Session()) { out.ok = false; out.error = "device heap profile could not be initialized"; return false; }
#else
        if (!mpDeviceAllocatorReset(mpDeviceRequestedState())) { out.ok = false; out.error = "device heap profile could not be initialized"; return false; }
#endif
        mpDeviceBeginRenderTelemetry();
        struct AllocationScope {
            AllocationScope() { mpDeviceSetAllocationActive(true); }
            ~AllocationScope() { mpDeviceSetAllocationActive(false); }
        } allocationScope;
        mpDeviceSetPhase(MpAllocationPhase::Source);
#endif
#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        MicroPatternsParser& parser = *g_m5Session.parser;
#else
        MicroPatternsParser parser;
#endif
        String source(scriptText.c_str());
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::Parse);
#endif
        auto t0 = std::chrono::steady_clock::now();
        const bool parsed = parser.parse(source);
        if (parsed) {
            mp_program_fingerprint(parser.program(), (const uint8_t*)scriptText.data(), scriptText.size());
#if defined(MP_DEVICE_WATCHY)
            source = String();
#endif
            out.counters.programBytes = parser.getProgram().byteSize();
        }
        out.timings.parseMs = msSince(t0);
        if (!parsed) {
#if MP_DEVICE_CONSTRAINTS
            mpDeviceSetAllocationActive(false);
#endif
            out.ok = false;
            out.error = "parse failed:";
            for (const String& e : parser.getErrors()) { out.error += "\n  "; out.error += e.c_str(); }
            parser.reset();
            return false;
        }
        // The stored form is the file on flash, so it lives in host memory.
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetAllocationActive(false);
#endif
        const bool serialized = mp_program_serialize(parser.getProgram(), outBytes);
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetAllocationActive(true);
#endif
        parser.reset();
        if (!serialized) { out.ok = false; out.error = "program serialization failed"; return false; }
        out.counters.programFileBytes = outBytes.size();
        out.ok = true;
        return true;
    }

    void runStored(const std::vector<uint8_t>& bytes, int width, int height,
                   const RenderSeed& seed, RenderResult& out) override {
        auto tTotal = std::chrono::steady_clock::now();
        hostSetCanvasSize(width, height);
        DisplayManager displayMgr;
        displayMgr.initializeEPD();
#if MP_DEVICE_CONSTRAINTS
#if defined(MP_DEVICE_M5PAPER)
        if (!ensureM5Session()) {
#else
        if (!mpDeviceAllocatorReset(mpDeviceRequestedState())) {
#endif
            out.ok = false; out.error = "device heap profile could not be initialized"; return;
        }
        mpDeviceBeginRenderTelemetry();
        struct AllocationScope {
            AllocationScope() { mpDeviceSetAllocationActive(true); }
            ~AllocationScope() { mpDeviceSetAllocationActive(false); }
        } allocationScope;
        mpDeviceSetPhase(MpAllocationPhase::Parse); // "load" -- the front half, minus any parsing
#endif
        MpProgram loaded;
        auto t0 = std::chrono::steady_clock::now();
        String why;
        if (!mp_program_deserialize(bytes.data(), bytes.size(), loaded, &why)) {
            out.ok = false; out.error = std::string("stored program rejected: ") + why.c_str(); return;
        }
        out.timings.loadMs = msSince(t0);
        out.counters.programBytes = loaded.byteSize();
        out.counters.programFileBytes = bytes.size();
        generateAndRasterize(loaded, displayMgr, width, height, seed, out, tTotal);
    }

private:
    // Shared tail of run() / runStored(): runtime, rasterizer, readback.
    void generateAndRasterize(const MpProgram& program, DisplayManager& displayMgr,
                              int width, int height, const RenderSeed& seed,
                              RenderResult& out, std::chrono::steady_clock::time_point tTotal) {
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::DisplayList);
#endif
#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        delete g_m5Session.runtime;
        g_m5Session.runtime = nullptr;
        g_m5Session.runtime = new MicroPatternsRuntime(displayMgr.getWidth(), displayMgr.getHeight(), program);
        MicroPatternsRuntime& runtime = *g_m5Session.runtime;
#else
        MicroPatternsRuntime runtime(displayMgr.getWidth(), displayMgr.getHeight(), program);
#endif
        runtime.setCounter(seed.counter);
        runtime.setTime(seed.hour, seed.minute, seed.second);
        auto t0 = std::chrono::steady_clock::now();
        runtime.generateDisplayList();
        out.timings.displayListMs = msSince(t0);

#if MP_DEVICE_CONSTRAINTS && defined(MP_DEVICE_M5PAPER)
        delete g_m5Session.renderer;
        g_m5Session.renderer = nullptr;
        g_m5Session.renderer = new DisplayListRenderer(displayMgr.getCanvas(), displayMgr.getWidth(), displayMgr.getHeight());
        DisplayListRenderer& renderer = *g_m5Session.renderer;
#else
        DisplayListRenderer renderer(displayMgr.getCanvas(), displayMgr.getWidth(), displayMgr.getHeight());
#endif
        renderer.setOcclusionEnabled(_occlusion);
        renderer.setOccupancyMapEnabled(_occupancyMap);
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetPhase(MpAllocationPhase::Rasterize);
#endif
        t0 = std::chrono::steady_clock::now();
        renderer.render(runtime.getDisplayList());
        out.timings.rasterizeMs = msSince(t0);
        out.timings.totalMs = msSince(tTotal);

        out.counters.displayListItems = (int)runtime.getDisplayList().size();
        out.counters.displayListBytes = runtime.getDisplayList().size() * sizeof(DisplayListItem);
        out.counters.totalItems = renderer.getTotalItems();
        out.counters.renderedItems = renderer.getRenderedItems();
        out.counters.culledOffScreen = renderer.getCulledOffScreen();
        out.counters.culledByOcclusion = renderer.getCulledByOcclusion();
        out.counters.overdrawSkippedPixels = renderer.getOverdrawSkippedPixels();
        out.counters.occupancyMapUsed = renderer.usedOccupancyMapLastRender();
#if MP_DEVICE_CONSTRAINTS
        mpDeviceSetAllocationActive(false);
        mpDeviceSetPhase(MpAllocationPhase::Output);
#endif
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

    bool _occlusion;
    bool _occupancyMap;
    bool _roundtrip;
    const char* _name;
};

} // namespace

#if MP_DEVICE_CONSTRAINTS
void resetDeviceRenderSessionAfterFailure()
{
#if defined(MP_DEVICE_M5PAPER)
    destroyM5Objects();
#else
    mpDeviceSetAllocationActive(false);
#endif
}
#endif

std::unique_ptr<RenderPath> makeRenderPath(const std::string& name) {
    if (name == "displaylist") return std::unique_ptr<RenderPath>(new DisplayListPath(true));
    if (name == "displaylist-noocc") return std::unique_ptr<RenderPath>(new DisplayListPath(false));
    // What a Watchy renders when it cannot allocate the occupancy map.
    if (name == "displaylist-nomap") return std::unique_ptr<RenderPath>(new DisplayListPath(true, false));
    // The device path: compile, serialize, deserialize, run.
    if (name == "compiled") return std::unique_ptr<RenderPath>(new DisplayListPath(true, true, true));
    return nullptr;
}

std::vector<std::string> availableRenderPaths() {
    return {"displaylist", "displaylist-noocc", "displaylist-nomap", "compiled"};
}
