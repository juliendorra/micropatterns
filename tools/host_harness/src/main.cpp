// mpharness -- host-side test & benchmark harness for the MicroPatterns
// C++ renderer core.
//
// See tools/host_harness/README.md. Short version:
//   * This is NOT a device simulator. Host timings compare ALGORITHMS, not
//     devices. The device logs its own real split in render_controller.cpp.
//   * `verify` is the automated form of commit d427b02 "All path gives same
//     result": the equivalence gate that must pass before any benchmark
//     comparison means anything.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "image_io.h"
#include "render_path.h"

namespace {

const int kDefaultWidth = 960;   // M5Paper landscape
const int kDefaultHeight = 540;

// Fixed seeds. Deterministic on purpose: same seed -> byte-identical output.
struct NamedSeed { const char* tag; RenderSeed seed; };
const NamedSeed kSeeds[] = {
    {"c0_123456",   {0,  12, 34, 56}},
    {"c7_000000",   {7,   0,  0,  0}},
    {"c42_235959",  {42, 23, 59, 59}},
};
const int kNumSeeds = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

std::string baseNoExt(const std::string& p) {
    size_t s = p.find_last_of('/');
    std::string b = (s == std::string::npos) ? p : p.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

bool readTextFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

std::vector<std::string> listCorpus(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".mp") == 0) out.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

void mkdirp(const std::string& path) { mkdir(path.c_str(), 0755); }

// ---------------------------------------------------------------------------
// Tiny JSON emit / parse (only what bench+compare need).
// ---------------------------------------------------------------------------
struct BenchEntry {
    std::string script;
    std::string seedTag;
    std::vector<double> parse, dl, raster, total;
    RenderCounters counters;
};

double vmin(std::vector<double> v) { return *std::min_element(v.begin(), v.end()); }
double vmean(const std::vector<double>& v) {
    double s = 0; for (double x : v) s += x; return s / v.size();
}
double vmedian(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Flat "script|seed|phase" -> {min,median,mean} map, used by both writer and
// reader so compare only has to deal with one shape.
struct Stat { double min = 0, median = 0, mean = 0; };

void writeBenchJson(const std::string& path, const std::vector<BenchEntry>& entries,
                    const std::string& pathName, int reps) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    fprintf(f, "{\n  \"harness\": \"mpharness\",\n  \"render_path\": \"%s\",\n  \"reps\": %d,\n",
            pathName.c_str(), reps);
    fprintf(f, "  \"note\": \"HOST timings (x86-64/ARM). NOT ESP32 wall-clock. Use for relative algorithmic comparison only.\",\n");
    fprintf(f, "  \"cases\": [\n");
    for (size_t i = 0; i < entries.size(); ++i) {
        const BenchEntry& e = entries[i];
        fprintf(f, "    {\"script\": \"%s\", \"seed\": \"%s\",\n", e.script.c_str(), e.seedTag.c_str());
        struct { const char* k; const std::vector<double>* v; } ph[4] = {
            {"parse", &e.parse}, {"displaylist", &e.dl}, {"rasterize", &e.raster}, {"total", &e.total}};
        for (int p = 0; p < 4; ++p) {
            fprintf(f, "     \"%s\": {\"min\": %.6f, \"median\": %.6f, \"mean\": %.6f},\n",
                    ph[p].k, vmin(*ph[p].v), vmedian(*ph[p].v), vmean(*ph[p].v));
        }
        fprintf(f, "     \"counters\": {\"display_list_items\": %d, \"rendered\": %d, "
                   "\"culled_offscreen\": %d, \"culled_occlusion\": %d, "
                   "\"overdraw_skipped_pixels\": %u, \"non_white_pixels\": %ld}}%s\n",
                e.counters.displayListItems, e.counters.renderedItems,
                e.counters.culledOffScreen, e.counters.culledByOcclusion,
                e.counters.overdrawSkippedPixels, e.counters.nonWhitePixels,
                (i + 1 == entries.size()) ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

// Extremely small scraper matched to the exact shape writeBenchJson emits.
bool readBenchJson(const std::string& path, std::map<std::string, Stat>& out, std::string& pathName) {
    std::string txt;
    if (!readTextFile(path, txt)) { fprintf(stderr, "cannot read %s\n", path.c_str()); return false; }
    size_t rp = txt.find("\"render_path\": \"");
    if (rp != std::string::npos) {
        rp += 16;
        pathName = txt.substr(rp, txt.find('"', rp) - rp);
    }
    size_t pos = 0;
    std::string script, seed;
    const char* phases[4] = {"parse", "displaylist", "rasterize", "total"};
    while (true) {
        size_t sp = txt.find("{\"script\": \"", pos);
        if (sp == std::string::npos) break;
        sp += 12;
        script = txt.substr(sp, txt.find('"', sp) - sp);
        size_t sd = txt.find("\"seed\": \"", sp);
        sd += 9;
        seed = txt.substr(sd, txt.find('"', sd) - sd);
        size_t caseEnd = txt.find("{\"script\": \"", sd);
        for (int p = 0; p < 4; ++p) {
            std::string key = std::string("\"") + phases[p] + "\": {\"min\": ";
            size_t kp = txt.find(key, sd);
            if (kp == std::string::npos || (caseEnd != std::string::npos && kp > caseEnd)) continue;
            Stat s;
            sscanf(txt.c_str() + kp + key.size(), "%lf", &s.min);
            size_t mp = txt.find("\"median\": ", kp);
            sscanf(txt.c_str() + mp + 10, "%lf", &s.median);
            size_t ap = txt.find("\"mean\": ", kp);
            sscanf(txt.c_str() + ap + 8, "%lf", &s.mean);
            out[script + "|" + seed + "|" + phases[p]] = s;
        }
        pos = sd;
    }
    return true;
}

// ---------------------------------------------------------------------------
void printBanner() {
    printf("mpharness -- HOST harness. Timings compare ALGORITHMS, not devices.\n"
           "             Host numbers are NOT ESP32 wall-clock. See README.md.\n\n");
}

int diffImages(const GrayImage& a, const GrayImage& b, GrayImage* diffOut,
               int& firstX, int& firstY) {
    firstX = firstY = -1;
    if (a.width != b.width || a.height != b.height) return -1;
    int n = 0;
    if (diffOut) {
        diffOut->width = a.width;
        diffOut->height = a.height;
        diffOut->pixels.assign(a.pixels.size(), 255);
    }
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        if (a.pixels[i] != b.pixels[i]) {
            if (n == 0) { firstX = (int)(i % a.width); firstY = (int)(i / a.width); }
            n++;
            if (diffOut) diffOut->pixels[i] = 0;
        }
    }
    return n;
}

struct Opts {
    std::string corpus, goldenDir, out, jsonOut, pathName = "displaylist";
    int width = kDefaultWidth, height = kDefaultHeight;
    int counter = 0, hour = 12, minute = 34, second = 56;
    int reps = 5;
    int draws = 0;   // `cycle` mode: number of $COUNTER values to render
    bool quiet = false;
};

// Resolve default corpus/golden dirs relative to the executable's location so
// the tool works from any cwd.
std::string g_root = ".";

int usage() {
    printf(
      "Usage: mpharness <mode> [options]\n"
      "\n"
      "Modes:\n"
      "  render <script.mp>      Render one script; print phase timings + counters.\n"
      "      --counter N --hour H --minute M --second S   (deterministic seed)\n"
      "      --width W --height H   (default 960x540, M5Paper landscape)\n"
      "      --out FILE             .png or .pgm (default out/<name>.png)\n"
      "      --path NAME            render path (default displaylist)\n"
      "  verify                  Re-render corpus at fixed seeds, byte-compare to goldens.\n"
      "  bake                    (Re)generate goldens. READ THE README BEFORE USING.\n"
      "  bench                   Run corpus N times; per-script + aggregate min/median/mean.\n"
      "      --reps N --json FILE\n"
      "  compare <a.json> <b.json>   Percentage deltas between two bench runs.\n"
      "  compare-paths <A> <B>       Byte-compare two render paths over the corpus.\n"
      "  list                    List corpus scripts, seeds and available render paths.\n"
      "  cycle <script.mp>       Render N counters; report distinct frames and repeat period.\n"
      "      --draws N              (default 64)\n"
      "\n"
      "Common options: --corpus DIR  --golden DIR  --quiet\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string mode = argv[1];

    // Locate the tree root (tools/host_harness) from argv[0], which is normally
    // build/mpharness. Overridden by explicit --corpus / --golden / --out.
    g_root = dirOf(argv[0]) + "/..";
    {   // collapse the trailing "<dir>/.." for readable paths in output
        size_t s = g_root.size();
        if (s > 3 && g_root.compare(s - 3, 3, "/..") == 0) {
            std::string head = g_root.substr(0, s - 3);
            size_t sl = head.find_last_of('/');
            g_root = (sl == std::string::npos) ? std::string(".") : head.substr(0, sl);
        }
    }
    Opts o;
    o.corpus = g_root + "/corpus";
    o.goldenDir = g_root + "/golden";

    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--corpus") o.corpus = next();
        else if (a == "--golden") o.goldenDir = next();
        else if (a == "--out") o.out = next();
        else if (a == "--json") o.jsonOut = next();
        else if (a == "--path") o.pathName = next();
        else if (a == "--width") o.width = atoi(next().c_str());
        else if (a == "--height") o.height = atoi(next().c_str());
        else if (a == "--counter") o.counter = atoi(next().c_str());
        else if (a == "--hour") o.hour = atoi(next().c_str());
        else if (a == "--minute") o.minute = atoi(next().c_str());
        else if (a == "--second") o.second = atoi(next().c_str());
        else if (a == "--reps") o.reps = atoi(next().c_str());
        else if (a == "--draws") o.draws = atoi(next().c_str());
        else if (a == "--quiet") o.quiet = true;
        else if (a == "-h" || a == "--help") return usage();
        else positional.push_back(a);
    }

    if (mode == "list") {
        printBanner();
        printf("Corpus dir : %s\n", o.corpus.c_str());
        for (const std::string& s : listCorpus(o.corpus)) printf("  %s\n", baseNoExt(s).c_str());
        printf("Seeds:\n");
        for (int i = 0; i < kNumSeeds; ++i)
            printf("  %-12s counter=%d %02d:%02d:%02d\n", kSeeds[i].tag, kSeeds[i].seed.counter,
                   kSeeds[i].seed.hour, kSeeds[i].seed.minute, kSeeds[i].seed.second);
        printf("Render paths:\n");
        for (const std::string& p : availableRenderPaths()) printf("  %s\n", p.c_str());
        return 0;
    }

    if (mode == "compare") {
        if (positional.size() < 2) return usage();
        std::map<std::string, Stat> A, B;
        std::string pa, pb;
        if (!readBenchJson(positional[0], A, pa)) return 1;
        if (!readBenchJson(positional[1], B, pb)) return 1;
        printBanner();
        printf("A = %s (path %s)\nB = %s (path %s)\n\n", positional[0].c_str(), pa.c_str(),
               positional[1].c_str(), pb.c_str());
        printf("%-42s %12s %12s %9s\n", "case|phase", "A median ms", "B median ms", "delta");
        printf("%s\n", std::string(78, '-').c_str());
        double sumA = 0, sumB = 0;
        for (const auto& kv : A) {
            auto it = B.find(kv.first);
            if (it == B.end()) continue;
            double a = kv.second.median, b = it->second.median;
            double pct = (a > 0) ? (b - a) / a * 100.0 : 0.0;
            if (kv.first.size() > 6 && kv.first.compare(kv.first.size() - 5, 5, "total") == 0) {
                sumA += a; sumB += b;
            }
            printf("%-42s %12.4f %12.4f %+8.2f%%\n", kv.first.c_str(), a, b, pct);
        }
        printf("%s\n", std::string(78, '-').c_str());
        printf("%-42s %12.4f %12.4f %+8.2f%%\n", "AGGREGATE total", sumA, sumB,
               sumA > 0 ? (sumB - sumA) / sumA * 100.0 : 0.0);
        printf("\nReminder: equivalence first. A speedup that changes the image is not a\n"
               "speedup -- run `mpharness verify` before trusting any delta above.\n");
        return 0;
    }

    std::unique_ptr<RenderPath> path = makeRenderPath(o.pathName);
    if (!path) { fprintf(stderr, "unknown render path: %s\n", o.pathName.c_str()); return 2; }

    // ---------------- render -------------------------------------------------
    if (mode == "render") {
        if (positional.empty()) return usage();
        std::string script;
        if (!readTextFile(positional[0], script)) {
            fprintf(stderr, "cannot read script: %s\n", positional[0].c_str());
            return 1;
        }
        RenderSeed seed{o.counter, o.hour, o.minute, o.second};
        RenderResult r;
        path->run(script, o.width, o.height, seed, r);
        if (!r.ok) { fprintf(stderr, "render failed: %s\n", r.error.c_str()); return 1; }

        std::string outPath = o.out.empty()
            ? (g_root + "/out/" + baseNoExt(positional[0]) + ".png") : o.out;
        mkdirp(dirOf(outPath));
        std::string err;
        if (!writeImage(outPath, r.image, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

        if (!o.quiet) {
            printBanner();
            printf("script      : %s\n", positional[0].c_str());
            printf("path        : %s\n", path->name());
            printf("canvas      : %dx%d\n", o.width, o.height);
            printf("seed        : counter=%d time=%02d:%02d:%02d\n", seed.counter, seed.hour, seed.minute, seed.second);
            printf("output      : %s\n\n", outPath.c_str());
            double t = r.timings.totalMs;
            printf("PHASE TIMINGS (host, single run)\n");
            printf("  parse           %10.3f ms   %5.1f%%\n", r.timings.parseMs, 100.0 * r.timings.parseMs / t);
            printf("  displaylist     %10.3f ms   %5.1f%%\n", r.timings.displayListMs, 100.0 * r.timings.displayListMs / t);
            printf("  rasterize       %10.3f ms   %5.1f%%\n", r.timings.rasterizeMs, 100.0 * r.timings.rasterizeMs / t);
            printf("  ------------------------------------\n");
            printf("  total           %10.3f ms\n\n", t);
            printf("COUNTERS\n");
            printf("  display list items       %10d\n", r.counters.displayListItems);
            printf("  rendered items           %10d\n", r.counters.renderedItems);
            printf("  culled off-screen        %10d\n", r.counters.culledOffScreen);
            printf("  culled by occlusion      %10d\n", r.counters.culledByOcclusion);
            printf("  pixels skipped (occ.map) %10u\n", r.counters.overdrawSkippedPixels);
            printf("  non-white pixels         %10ld   (coverage proxy, not draw calls)\n", r.counters.nonWhitePixels);
        }
        return 0;
    }

    // ---------------- verify / bake -----------------------------------------
    // Does a script actually VARY as $COUNTER advances?
    //
    // Every gate above compares single frames for equality -- against a golden,
    // against another render path, against the other language's renderer. None
    // of them can see a script that renders a byte-perfect frame every time and
    // yet only ever shows four of its ten variations, or repeats itself every
    // 32 draws. There are no wrong pixels in that failure; there is only a
    // distribution, and it is invisible one frame at a time.
    //
    // The usual cause is a pseudo-random sequence read through its low bits.
    // An LCG with a power-of-two modulus -- including the implicit 2^32 of int
    // arithmetic -- has bit k repeating with period 2^(k+1). Verified: bits 0-7
    // have periods 2, 4, 8, 16, 32, 64, 128, 256. So ($seed / 8) % 4 reads bits
    // 3 and 4 and cannot do better than period 32, however many draws you take.
    // Reducing modulo a PRIME (32749 rather than 32768) mixes the high bits
    // back in and breaks the pattern.
    if (mode == "cycle") {
        if (positional.empty()) return usage();
        std::string script;
        if (!readTextFile(positional[0], script)) {
            fprintf(stderr, "cannot read script: %s\n", positional[0].c_str());
            return 1;
        }
        const int draws = o.draws > 0 ? o.draws : 64;

        std::vector<uint64_t> hashes;
        hashes.reserve(draws);
        for (int c = 0; c < draws; ++c) {
            RenderSeed seed{c, o.hour, o.minute, o.second};
            RenderResult r;
            path->run(script, o.width, o.height, seed, r);
            if (!r.ok) { fprintf(stderr, "render failed at counter %d: %s\n", c, r.error.c_str()); return 1; }
            // FNV-1a over the frame. Only equality matters here.
            uint64_t h = 1469598103934665603ULL;
            for (uint8_t px : r.image.pixels) { h ^= px; h *= 1099511628211ULL; }
            hashes.push_back(h);
        }

        std::set<uint64_t> distinct(hashes.begin(), hashes.end());

        // Smallest p such that frame[i] == frame[i+p] for every i we can check.
        int periodFound = 0;
        for (int p = 1; p <= draws / 2; ++p) {
            bool ok = true;
            for (int i = 0; i + p < draws && ok; ++i) if (hashes[i] != hashes[i + p]) ok = false;
            if (ok) { periodFound = p; break; }
        }

        printf("VARIATION over %d counters (time fixed at %02d:%02d:%02d)\n\n",
               draws, o.hour, o.minute, o.second);
        printf("  distinct frames      %d of %d\n", (int)distinct.size(), draws);
        if (periodFound) printf("  repeats every        %d draws\n", periodFound);
        else             printf("  repeats every        no repeat within %d draws\n", draws);

        if (periodFound && periodFound < draws) {
            printf("\n  This script cycles. If that is not deliberate, suspect a\n"
                   "  pseudo-random value read through its low bits -- a power-of-two\n"
                   "  modulus gives bit k a period of only 2^(k+1). Reduce modulo a\n"
                   "  prime instead (32749, not 32768).\n");
            return 1;
        }
        // Deliberately NOT failing on a low distinct-frame count. A script that
        // legitimately picks one of four positions has four distinct frames
        // however many times it is drawn, and flagging that would make this
        // tool cry wolf on correct scripts. The repeat PERIOD is the signal;
        // the count is context.
        return 0;
    }

    if (mode == "verify" || mode == "bake") {
        std::vector<std::string> scripts = listCorpus(o.corpus);
        if (scripts.empty()) { fprintf(stderr, "no .mp scripts in %s\n", o.corpus.c_str()); return 1; }
        mkdirp(o.goldenDir);
        if (!o.quiet) printBanner();
        if (mode == "bake") {
            printf("BAKE: regenerating goldens in %s\n", o.goldenDir.c_str());
            printf("      Regenerating goldens to make a failing test pass DEFEATS THE PURPOSE.\n"
                   "      Only bake when the output change is intended and reviewed.\n\n");
        } else {
            printf("VERIFY: golden-image equivalence gate (the automated form of\n"
                   "        commit d427b02 \"All path gives same result\").\n\n");
        }

        int pass = 0, fail = 0;
        for (const std::string& sp : scripts) {
            std::string script;
            if (!readTextFile(sp, script)) { fprintf(stderr, "cannot read %s\n", sp.c_str()); fail++; continue; }
            for (int si = 0; si < kNumSeeds; ++si) {
                std::string caseName = baseNoExt(sp) + "__" + kSeeds[si].tag;
                std::string gpath = o.goldenDir + "/" + caseName + ".pgm";
                RenderResult r;
                path->run(script, o.width, o.height, kSeeds[si].seed, r);
                if (!r.ok) {
                    printf("  FAIL  %-34s render error: %s\n", caseName.c_str(), r.error.c_str());
                    fail++;
                    continue;
                }
                std::string err;
                if (mode == "bake") {
                    if (!writePGM(gpath, r.image, err)) { printf("  ERROR %s: %s\n", caseName.c_str(), err.c_str()); fail++; }
                    else { printf("  baked %-34s %d items, %ld ink px\n", caseName.c_str(),
                                  r.counters.displayListItems, r.counters.nonWhitePixels); pass++; }
                    continue;
                }
                GrayImage golden;
                if (!readPGM(gpath, golden, err)) {
                    printf("  MISS  %-34s no golden (%s) -- run `bake`\n", caseName.c_str(), err.c_str());
                    fail++;
                    continue;
                }
                int fx, fy;
                int nd = diffImages(golden, r.image, nullptr, fx, fy);
                if (nd == 0) { printf("  PASS  %-34s\n", caseName.c_str()); pass++; }
                else if (nd < 0) {
                    printf("  FAIL  %-34s dimension mismatch golden %dx%d vs %dx%d\n",
                           caseName.c_str(), golden.width, golden.height, r.image.width, r.image.height);
                    fail++;
                } else {
                    GrayImage d;
                    diffImages(golden, r.image, &d, fx, fy);
                    std::string dp = g_root + "/out/diff_" + caseName + ".png";
                    mkdirp(dirOf(dp));
                    std::string e2;
                    writePNG(dp, d, e2);
                    printf("  FAIL  %-34s %d differing px (%.4f%%), first at (%d,%d), diff -> %s\n",
                           caseName.c_str(), nd, 100.0 * nd / (double)golden.pixels.size(), fx, fy, dp.c_str());
                    fail++;
                }
            }
        }
        printf("\n%s: %d passed, %d failed\n", mode == "bake" ? "BAKE" : "VERIFY", pass, fail);
        return fail ? 1 : 0;
    }

    // ---------------- compare-paths -----------------------------------------
    if (mode == "compare-paths") {
        if (positional.size() < 2) {
            fprintf(stderr, "compare-paths needs two render path names. Available:\n");
            for (const std::string& p : availableRenderPaths()) fprintf(stderr, "  %s\n", p.c_str());
            return 2;
        }
        auto A = makeRenderPath(positional[0]);
        auto B = makeRenderPath(positional[1]);
        if (!A || !B) { fprintf(stderr, "unknown render path\n"); return 2; }
        printBanner();
        printf("PATH EQUIVALENCE: %s vs %s\n", A->name(), B->name());
        printf("(This is the \"All path gives same result\" gate, in C++.)\n\n");
        int pass = 0, fail = 0;
        for (const std::string& sp : listCorpus(o.corpus)) {
            std::string script;
            if (!readTextFile(sp, script)) continue;
            for (int si = 0; si < kNumSeeds; ++si) {
                RenderResult ra, rb;
                A->run(script, o.width, o.height, kSeeds[si].seed, ra);
                B->run(script, o.width, o.height, kSeeds[si].seed, rb);
                std::string caseName = baseNoExt(sp) + "__" + kSeeds[si].tag;
                if (!ra.ok || !rb.ok) { printf("  FAIL  %-34s render error\n", caseName.c_str()); fail++; continue; }
                int fx, fy;
                int nd = diffImages(ra.image, rb.image, nullptr, fx, fy);
                if (nd == 0) { printf("  SAME  %-34s\n", caseName.c_str()); pass++; }
                else { printf("  DIFF  %-34s %d px, first (%d,%d)\n", caseName.c_str(), nd, fx, fy); fail++; }
            }
        }
        printf("\n%d identical, %d differing\n", pass, fail);
        return fail ? 1 : 0;
    }

    // ---------------- bench ---------------------------------------------------
    if (mode == "bench") {
        std::vector<std::string> scripts = listCorpus(o.corpus);
        if (scripts.empty()) { fprintf(stderr, "no .mp scripts in %s\n", o.corpus.c_str()); return 1; }
        printBanner();
        printf("BENCH: %zu scripts x %d seeds x %d reps, path=%s, canvas %dx%d\n\n",
               scripts.size(), kNumSeeds, o.reps, path->name(), o.width, o.height);

        std::vector<BenchEntry> entries;
        for (const std::string& sp : scripts) {
            std::string script;
            if (!readTextFile(sp, script)) continue;
            for (int si = 0; si < kNumSeeds; ++si) {
                BenchEntry e;
                e.script = baseNoExt(sp);
                e.seedTag = kSeeds[si].tag;
                for (int r = 0; r < o.reps; ++r) {
                    RenderResult rr;
                    path->run(script, o.width, o.height, kSeeds[si].seed, rr);
                    if (!rr.ok) { fprintf(stderr, "bench: %s failed: %s\n", e.script.c_str(), rr.error.c_str()); break; }
                    e.parse.push_back(rr.timings.parseMs);
                    e.dl.push_back(rr.timings.displayListMs);
                    e.raster.push_back(rr.timings.rasterizeMs);
                    e.total.push_back(rr.timings.totalMs);
                    e.counters = rr.counters;
                }
                if (e.total.empty()) continue;
                entries.push_back(e);
            }
        }

        printf("%-28s %10s %10s %10s %10s %8s\n", "case", "parse", "displist", "raster", "total", "items");
        printf("%s\n", std::string(82, '-').c_str());
        double aggP = 0, aggD = 0, aggR = 0, aggT = 0;
        for (const BenchEntry& e : entries) {
            printf("%-28s %10.3f %10.3f %10.3f %10.3f %8d\n",
                   (e.script + "|" + e.seedTag).c_str(),
                   vmedian(e.parse), vmedian(e.dl), vmedian(e.raster), vmedian(e.total),
                   e.counters.displayListItems);
            aggP += vmedian(e.parse); aggD += vmedian(e.dl);
            aggR += vmedian(e.raster); aggT += vmedian(e.total);
        }
        printf("%s\n", std::string(82, '-').c_str());
        printf("%-28s %10.3f %10.3f %10.3f %10.3f\n", "SUM of medians", aggP, aggD, aggR, aggT);
        if (aggT > 0) {
            printf("%-28s %9.1f%% %9.1f%% %9.1f%%\n", "PHASE SPLIT",
                   100.0 * aggP / aggT, 100.0 * aggD / aggT, 100.0 * aggR / aggT);
        }
        // Per-case split matters more than the aggregate: the aggregate is
        // dominated by whichever script is slowest, and the split is strongly
        // script-dependent. Print both, and say so.
        printf("\nPER-CASE PHASE SPLIT (%% of that case's median total)\n");
        printf("%-28s %10s %10s %10s\n", "case", "parse", "displist", "raster");
        printf("%s\n", std::string(60, '-').c_str());
        for (const BenchEntry& e : entries) {
            double t = vmedian(e.total);
            if (t <= 0) continue;
            printf("%-28s %9.1f%% %9.1f%% %9.1f%%\n", (e.script + "|" + e.seedTag).c_str(),
                   100.0 * vmedian(e.parse) / t, 100.0 * vmedian(e.dl) / t, 100.0 * vmedian(e.raster) / t);
        }

        printf("\nmin/median/mean per case:\n");
        for (const BenchEntry& e : entries) {
            printf("  %-26s total  min %8.3f  med %8.3f  mean %8.3f  (n=%zu)\n",
                   (e.script + "|" + e.seedTag).c_str(), vmin(e.total), vmedian(e.total),
                   vmean(e.total), e.total.size());
        }
        if (!o.jsonOut.empty()) {
            mkdirp(dirOf(o.jsonOut));
            writeBenchJson(o.jsonOut, entries, path->name(), o.reps);
            printf("\nwrote %s\n", o.jsonOut.c_str());
        }
        return 0;
    }

    return usage();
}
