// Host shim for FreeRTOS types referenced by the firmware's display_manager.h.
// Nothing here is functional; it exists so the real header parses on the host.
#ifndef HOST_SHIM_FREERTOS_H
#define HOST_SHIM_FREERTOS_H

#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif // HOST_SHIM_FREERTOS_H
