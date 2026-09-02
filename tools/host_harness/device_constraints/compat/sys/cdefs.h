// ESP-IDF's portable multi_heap source includes newlib's <sys/cdefs.h> but does
// not use any declaration from it in the host configuration. Emscripten has no
// such header, so this intentionally empty compatibility header preserves the
// upstream source file unchanged.
#pragma once

