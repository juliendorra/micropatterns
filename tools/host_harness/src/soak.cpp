// mpsoak -- lifetime and memory-safety soak for the renderer core.
//
// `verify` proves the renderer draws the right picture. It does NOT prove the
// renderer is memory-safe, and it cannot: it renders each script once, from a
// freshly constructed parser and runtime, on a canvas whose out-of-range pixel
// writes the shim silently drops. Three whole classes of defect are invisible
// to it, and one of them (a display-list item pointing at a freed transform
// snapshot) was a live design hazard in the commit that introduced the pooled
// snapshots:
//
//   1. LIFETIME. DisplayListItem holds raw pointers -- into the runtime's
//      TransformSnapshot pool, and into the parser's asset map. Nothing in a
//      single-render test can tell a valid pointer from a stale one.
//   2. MEMO STALENESS. The runtime memoises variable slots and resolved assets
//      into `mutable` fields on the PARSE TREE, tagged with a per-runtime epoch.
//      A one-shot render never re-reads a memo written by a different runtime.
//   3. NON-DETERMINISM. Reading uninitialised or recycled memory usually still
//      produces *a* picture. Rendering the same input twice and comparing is
//      what turns that into a failure.
//
// So this binary runs the shape the M5Paper firmware actually uses -- see
// RenderController::renderScript: ONE parser reused across renders, reset and
// re-parsed each time, with the runtime and renderer deleted and recreated --
// for many renders, over many seeds, and byte-compares repeat renders of the
// same seed. Built with -fsanitize=address,undefined by `make soak`, so a
// dangling read or an out-of-range slot index is a hard failure rather than a
// wrong pixel somewhere.
//
// It deliberately takes scripts that have NO goldens. Golden coverage requires
// baking a reference image and is therefore expensive to add; sanitizer
// coverage requires only the script. That asymmetry is the point: anything
// procedural, adversarial, or generated can be soaked immediately.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "M5EPD.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_list_renderer.h"
#include "host_display_manager.h"

namespace {

struct Seed {
    int counter, hour, minute, second;
};

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path);
    if (!f) { ok = false; return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

std::string baseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// One render through the device-shaped path. `parser` is the caller's, reused.
bool renderOnce(MicroPatternsParser& parser, const std::string& text, const Seed& seed,
                int W, int H, M5EPD_Canvas& canvas, std::vector<uint8_t>& outPixels,
                int& outItems, std::string& err) {
    parser.reset();
    if (!parser.parse(String(text.c_str()))) {
        err = "parse failed:";
        for (const String& e : parser.getErrors()) { err += "\n    "; err += e.c_str(); }
        return false;
    }

    // Heap-allocated and destroyed in the same order RenderController uses, so
    // that a use-after-free of the snapshot pool or the asset map has the same
    // window here as it does on device.
    MicroPatternsRuntime* runtime =
        new MicroPatternsRuntime(W, H, parser.getAssets());
    runtime->setCommands(&parser.getCommands());
    runtime->setDeclaredVariables(&parser.getDeclaredVariables());
    runtime->setCounter(seed.counter);
    runtime->setTime(seed.hour, seed.minute, seed.second);
    runtime->generateDisplayList();
    outItems = (int)runtime->getDisplayList().size();

    DisplayListRenderer* renderer =
        new DisplayListRenderer(&canvas, parser.getAssets(), W, H);
    renderer->render(runtime->getDisplayList());

    outPixels = canvas.hostBuffer();

    delete renderer;
    delete runtime;
    return true;
}

int usage() {
    printf(
      "Usage: mpsoak [--width W] [--height H] [--renders N] <script.mp> [more.mp ...]\n"
      "\n"
      "Renders each script through the device-shaped path (one reused parser,\n"
      "runtime and renderer recreated per render) across N pseudo-random seeds,\n"
      "rendering every seed TWICE and byte-comparing the two canvases.\n"
      "\n"
      "Exit status is non-zero if any render is non-deterministic. Build it with\n"
      "sanitizers (`make soak`) so memory errors abort instead of being drawn.\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    int W = 540, H = 960;   // M5Paper portrait -- the larger of the two real canvases
    int renders = 24;
    std::vector<std::string> scripts;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--width") W = atoi(next().c_str());
        else if (a == "--height") H = atoi(next().c_str());
        else if (a == "--renders") renders = atoi(next().c_str());
        else if (a == "-h" || a == "--help") return usage();
        else scripts.push_back(a);
    }
    if (scripts.empty() || W <= 0 || H <= 0 || renders <= 0) return usage();

    hostSetCanvasSize(W, H);
    M5EPD_Canvas canvas;
    canvas.createCanvas(W, H);

    int failures = 0, totalRenders = 0;
    printf("SOAK: %d script(s), %d seeds each, %dx%d, two renders per seed\n\n",
           (int)scripts.size(), renders, W, H);

    for (const std::string& path : scripts) {
        bool ok = false;
        std::string text = readFile(path, ok);
        if (!ok) { printf("  FAIL  %-28s cannot read file\n", baseName(path).c_str()); failures++; continue; }

        // One parser for the whole script, reused across every render, exactly
        // as RenderController holds _parser across jobs.
        MicroPatternsParser parser;
        bool scriptOk = true;
        int minItems = -1, maxItems = -1;

        for (int r = 0; r < renders && scriptOk; ++r) {
            // Spread over counters, hours, minutes and seconds without a RNG so
            // a failure is reproducible from the seed index alone.
            Seed seed{ r * 13, (r * 7) % 24, (r * 11) % 60, (r * 17) % 60 };

            std::vector<uint8_t> a, b;
            int itemsA = 0, itemsB = 0;
            std::string err;

            if (!renderOnce(parser, text, seed, W, H, canvas, a, itemsA, err)) {
                printf("  FAIL  %-28s seed %d: %s\n", baseName(path).c_str(), r, err.c_str());
                scriptOk = false; break;
            }
            if (!renderOnce(parser, text, seed, W, H, canvas, b, itemsB, err)) {
                printf("  FAIL  %-28s seed %d (repeat): %s\n", baseName(path).c_str(), r, err.c_str());
                scriptOk = false; break;
            }
            totalRenders += 2;

            if (itemsA != itemsB) {
                printf("  FAIL  %-28s seed %d: display list size differs between identical renders "
                       "(%d vs %d)\n", baseName(path).c_str(), r, itemsA, itemsB);
                scriptOk = false; break;
            }
            if (a != b) {
                size_t at = 0;
                while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
                printf("  FAIL  %-28s seed %d (counter=%d %02d:%02d:%02d): identical renders produced "
                       "different pixels, first at byte %zu\n",
                       baseName(path).c_str(), r, seed.counter, seed.hour, seed.minute, seed.second, at);
                scriptOk = false; break;
            }
            if (minItems < 0 || itemsA < minItems) minItems = itemsA;
            if (itemsA > maxItems) maxItems = itemsA;
        }

        if (scriptOk) printf("  PASS  %-28s %d seeds, %d-%d items\n",
                             baseName(path).c_str(), renders, minItems, maxItems);
        else failures++;
    }

    printf("\nSOAK: %d script(s), %d renders, %d failed\n",
           (int)scripts.size(), totalRenders, failures);
    return failures == 0 ? 0 : 1;
}
