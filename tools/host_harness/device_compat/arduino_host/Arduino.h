// Minimal host surface used to compile the version-matched Arduino-ESP32
// WString implementation and the shared MicroPatterns rendering core.
// This file is only visible to constrained host/WASM builds.
#ifndef MP_DEVICE_COMPAT_ARDUINO_H
#define MP_DEVICE_COMPAT_ARDUINO_H

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <WString.h>

typedef bool boolean;
typedef uint8_t byte;

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

static inline void yield() {}

#endif
