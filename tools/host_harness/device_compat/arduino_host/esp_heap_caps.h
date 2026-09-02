// Minimal ESP-IDF heap_caps surface used by the shared renderer's device-only
// allocation guard. Values are intentionally opaque to the compatibility
// bridge: the current renderer asks only for MALLOC_CAP_8BIT.
#ifndef MP_DEVICE_COMPAT_ESP_HEAP_CAPS_H
#define MP_DEVICE_COMPAT_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1u << 2)

#ifdef __cplusplus
extern "C" {
#endif
size_t heap_caps_get_largest_free_block(uint32_t caps);
#ifdef __cplusplus
}
#endif

#endif
