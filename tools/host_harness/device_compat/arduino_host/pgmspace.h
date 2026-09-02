// ESP32 flash strings are ordinary read-only bytes in the wasm32 simulator.
#ifndef MP_DEVICE_COMPAT_PGMSPACE_H
#define MP_DEVICE_COMPAT_PGMSPACE_H

#include <cstring>

typedef const char* PGM_P;
#define PROGMEM
#define PSTR(value) (value)
#define memcpy_P(dst, src, count) memcpy((dst), (src), (count))
#define strlen_P(src) strlen((src))

#endif
