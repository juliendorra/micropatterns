// Host shim for <esp_task_wdt.h>. There is no watchdog on the host; the reset
// call must compile to nothing so it does not appear in benchmark profiles.
#ifndef HOST_SHIM_ESP_TASK_WDT_H
#define HOST_SHIM_ESP_TASK_WDT_H

static inline int esp_task_wdt_reset(void) { return 0; }
static inline int esp_task_wdt_add(void* handle) { (void)handle; return 0; }
static inline int esp_task_wdt_delete(void* handle) { (void)handle; return 0; }

#endif // HOST_SHIM_ESP_TASK_WDT_H
