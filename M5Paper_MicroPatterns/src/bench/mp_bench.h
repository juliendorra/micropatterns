// mp_bench.h -- on-device benchmark harness for the MicroPatterns M5Paper
// firmware (tier 2 of the test strategy; tier 1 is tools/host_harness).
//
// EVERYTHING in this header and its .cpp is compiled out unless MP_BENCH is
// defined. The default `pio run -e m5stack-fire` build never sees a byte of it:
// the whole file body is inside `#if MP_BENCH`, and the only hook in existing
// firmware is a single `#if MP_BENCH` block at the top of setup() in main.cpp.
//
// Build it with:   pio run -e m5paper-bench
//
// See docs/analysis/device-benchmark-harness.md for the full design, the
// canonical canvas-checksum byte layout, and how to collect results.
#ifndef MP_BENCH_H
#define MP_BENCH_H

#if MP_BENCH

#include <M5EPD.h>
#include <esp_timer.h>

// Marker prefix every machine-readable bench line starts with. Chosen so it
// survives a serial stream that is also full of ESP-IDF log output: a plain
// `grep '^MPBENCH|'` is enough to extract the results.
#define MPBENCH_MARKER "MPBENCH|"

// --- Tiny timing helper -----------------------------------------------------
// Deliberately one small type instead of millis() calls scattered through the
// render path. esp_timer_get_time() is microseconds; millis() cannot resolve a
// 3 ms parse.
struct MPBenchStopwatch {
    int64_t t0;
    MPBenchStopwatch() : t0(esp_timer_get_time()) {}
    inline void restart() { t0 = esp_timer_get_time(); }
    inline int64_t us() const { return esp_timer_get_time() - t0; }
};

// --- Canvas checksum --------------------------------------------------------
// FNV-1a 32 over the canonical 4bpp canvas byte stream. See the header comment
// in mp_bench.cpp and docs/analysis/device-benchmark-harness.md for the exact
// byte layout the host harness must reproduce.
uint32_t MPBench_CanvasFnv1a(M5EPD_Canvas* canvas, int width, int height,
                             uint32_t* outNonWhitePixels);

// Creates the benchmark task and returns. Never returns control to normal
// firmware operation: the bench task owns the device from here on.
void MPBench_Start();

#endif // MP_BENCH
#endif // MP_BENCH_H
