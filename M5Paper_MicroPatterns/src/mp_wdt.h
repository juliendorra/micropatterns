#ifndef MP_WDT_H
#define MP_WDT_H

// Feed the task watchdog, but ONLY if the calling task is subscribed to it.
//
// The renderer resets the watchdog periodically (every 8 scanlines). On the
// M5Paper that runs on RenderTask, which is subscribed. On the Watchy it runs
// on loopTask, which is not.
//
// On IDF 4.4 an unsubscribed reset was silently ignored. On IDF 5.3 it logs
//   E task_wdt: esp_task_wdt_reset(): task not found
// on EVERY call -- hundreds of serial writes per render, which measurably
// slowed rendering (~490ms -> ~1200ms on the Watchy).
//
// Subscribing loopTask instead was tried and is WORSE: the watchdog then
// genuinely fires during parsing, which does not feed it, and the device boot
// loops. So: check first, and stay a no-op where there is no watchdog to feed.
#include <cstddef>          // NULL, on the host shim path
#include <esp_task_wdt.h>

static inline void mp_wdt_reset()
{
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

#endif // MP_WDT_H
