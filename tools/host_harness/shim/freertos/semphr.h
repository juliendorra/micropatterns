// Host shim for FreeRTOS semaphores (see freertos/FreeRTOS.h).
// The harness is single-threaded; these are no-ops.
#ifndef HOST_SHIM_SEMPHR_H
#define HOST_SHIM_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef void* SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline void vSemaphoreDelete(SemaphoreHandle_t h) { (void)h; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { (void)h; (void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return pdTRUE; }

#endif // HOST_SHIM_SEMPHR_H
