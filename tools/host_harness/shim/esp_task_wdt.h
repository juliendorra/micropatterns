// Host shim for <esp_task_wdt.h>. There is no watchdog on the host; the reset
// call must compile to nothing so it does not appear in benchmark profiles.
#ifndef HOST_SHIM_ESP_TASK_WDT_H
#define HOST_SHIM_ESP_TASK_WDT_H

#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

static inline int esp_task_wdt_reset(void) { return 0; }
static inline int esp_task_wdt_add(void* handle) { (void)handle; return 0; }
static inline int esp_task_wdt_delete(void* handle) { (void)handle; return 0; }

// Reports whether the calling task is subscribed to the task watchdog. The
// renderer checks this before calling esp_task_wdt_reset(), because IDF 5.3
// logs an error on every reset from an unsubscribed task. There is no watchdog
// here, so report "not subscribed" and let mp_wdt_reset() be a no-op.
static inline int esp_task_wdt_status(void* handle) { (void)handle; return ESP_ERR_NOT_FOUND; }

#endif // HOST_SHIM_ESP_TASK_WDT_H
