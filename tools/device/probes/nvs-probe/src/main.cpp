// Minimal NVS test: nothing but NVS. No display, no BLE, no project code.
#include <Arduino.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"

void setup() {
    Serial.begin(115200);
    delay(2000);                      // let a monitor attach (InkWatchy does this)
    Serial.println("\n=== MINIMAL NVS TEST ===");

    esp_err_t e = nvs_flash_init();
    Serial.printf("nvs_flash_init: %s\n", esp_err_to_name(e));
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("erasing and retrying");
        nvs_flash_erase();
        e = nvs_flash_init();
        Serial.printf("nvs_flash_init retry: %s\n", esp_err_to_name(e));
    }

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    Serial.printf("partition: %s @0x%lx size 0x%lx\n",
                  p ? p->label : "NONE",
                  p ? (unsigned long)p->address : 0UL,
                  p ? (unsigned long)p->size : 0UL);

    nvs_handle_t h;
    esp_err_t eo = nvs_open("t", NVS_READWRITE, &h);
    Serial.printf("open: %s\n", esp_err_to_name(eo));
    if (eo == ESP_OK) {
        Serial.printf("set_u8:  %s\n", esp_err_to_name(nvs_set_u8(h, "k", 42)));
        uint8_t v = 0;
        Serial.printf("get BEFORE commit: %s val=%u\n",
                      esp_err_to_name(nvs_get_u8(h, "k", &v)), v);
        Serial.printf("commit:  %s\n", esp_err_to_name(nvs_commit(h)));
        v = 0;
        Serial.printf("get AFTER commit:  %s val=%u\n",
                      esp_err_to_name(nvs_get_u8(h, "k", &v)), v);
        nvs_close(h);

        // Reopen from scratch -- does it survive a close?
        nvs_handle_t h2;
        Serial.printf("reopen: %s\n", esp_err_to_name(nvs_open("t", NVS_READONLY, &h2)));
        v = 0;
        Serial.printf("get after reopen:  %s val=%u\n",
                      esp_err_to_name(nvs_get_u8(h2, "k", &v)), v);
        nvs_close(h2);
    }

    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
        Serial.printf("entries used=%u free=%u total=%u\n",
                      st.used_entries, st.free_entries, st.total_entries);
    Serial.println("=== END ===");
}
void loop() { delay(1000); }
