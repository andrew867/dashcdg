/*
 * dashcdg_badge — CYD 3.2" LVGL + touch Wi-Fi setup (no web portal; NVS credentials).
 * Protocol v4 RX layers hook in here later (docs/embedded/).
 */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "display_lvgl.h"
#include "wifi_touch_ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lv_disp_t *disp = NULL;
    ESP_ERROR_CHECK(dashcdg_display_lvgl_init(&disp));
    ESP_LOGI(TAG, "display OK");

    ESP_ERROR_CHECK(dashcdg_wifi_touch_ui_start(disp));
    ESP_LOGI(TAG, "wifi UI running");
}
