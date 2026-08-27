// Host shim for <esp32-hal-log.h>.
// Logging is OFF by default so it cannot distort benchmark timings.
// Enable at build time with -DHOST_LOG_LEVEL=n :
//   0 = silent (default), 1 = error, 2 = warn, 3 = info, 4 = debug, 5 = verbose
#ifndef HOST_SHIM_ESP32_HAL_LOG_H
#define HOST_SHIM_ESP32_HAL_LOG_H

#include <cstdio>

#ifndef HOST_LOG_LEVEL
#define HOST_LOG_LEVEL 0
#endif

#define HOST_LOG_(lvl, tag, fmt, ...)                                        \
    do {                                                                     \
        if ((lvl) <= HOST_LOG_LEVEL) {                                       \
            fprintf(stderr, "[" tag "] " fmt "\n", ##__VA_ARGS__);           \
        }                                                                    \
    } while (0)

#define log_e(fmt, ...) HOST_LOG_(1, "E", fmt, ##__VA_ARGS__)
#define log_w(fmt, ...) HOST_LOG_(2, "W", fmt, ##__VA_ARGS__)
#define log_i(fmt, ...) HOST_LOG_(3, "I", fmt, ##__VA_ARGS__)
#define log_d(fmt, ...) HOST_LOG_(4, "D", fmt, ##__VA_ARGS__)
#define log_v(fmt, ...) HOST_LOG_(5, "V", fmt, ##__VA_ARGS__)

#endif // HOST_SHIM_ESP32_HAL_LOG_H
