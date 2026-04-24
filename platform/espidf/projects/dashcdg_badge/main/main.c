/*
 * dashcdg_badge — CYD 3.2" LVGL + touch launcher (Wi-Fi, Karaoke stubs; CD-G multicast later).
 */
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "touch_cal_store.h"
#include "touch_cal_ui.h"
#include "vbat_sense.h"
#include "wifi_touch_ui.h"

static const char *TAG = "main";

#ifndef CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
#define CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION 0
#endif
#ifndef CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 0
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 0
#endif
#ifndef CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM 0
#endif
#ifndef CONFIG_ESP_WIFI_AMPDU_TX_ENABLED
#define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED 0
#endif
#ifndef CONFIG_ESP_WIFI_AMPDU_RX_ENABLED
#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED 0
#endif
#ifndef CONFIG_ESP_WIFI_MGMT_SBUF_NUM
#define CONFIG_ESP_WIFI_MGMT_SBUF_NUM 0
#endif

static void dashcdg_log_startup_runtime_cfg(void)
{
    const char *fr_static = "n";
    const char *fr_dynamic = "y";

#if CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
    fr_static = "y";
#endif
#if CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
    fr_dynamic = "n";
#endif

    ESP_LOGI(TAG, "cfg: freertos static=%s dynamic(inferred)=%s", fr_static, fr_dynamic);
    ESP_LOGI(TAG,
             "cfg: wifi static_rx=%d dynamic_rx=%d dynamic_tx=%d ampdu_tx=%d ampdu_rx=%d mgmt_sbuf=%d",
             CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM,
             CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM, CONFIG_ESP_WIFI_AMPDU_TX_ENABLED,
             CONFIG_ESP_WIFI_AMPDU_RX_ENABLED, CONFIG_ESP_WIFI_MGMT_SBUF_NUM);
}

static void dashcdg_show_boot_error_screen(lv_disp_t *disp, const char *stage, esp_err_t err)
{
    lv_obj_t *scr;
    lv_obj_t *lbl;
    char body[384];

    if (disp == NULL) {
        return;
    }
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "boot error (%s): %s (LVGL lock timeout)", stage, esp_err_to_name(err));
        return;
    }
    scr = lv_display_get_screen_active(disp);
    if (scr) {
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x120000), 0);
        lbl = lv_label_create(scr);
        lv_obj_set_width(lbl, lv_pct(96));
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffaa88), 0);
        snprintf(body, sizeof(body),
                 "[ boot error ]\n"
                 "stage: %s\n"
                 "err: %s\n"
                 "cfg: static=%d dyn(inferred)=%d\n"
                 "wifi rx/tx: %d/%d/%d\n"
                 "ampdu tx/rx: %d/%d\n"
                 "check UART log for details",
                 stage, esp_err_to_name(err), CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION,
                 CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION ? 0 : 1, CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM,
                 CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM,
                 CONFIG_ESP_WIFI_AMPDU_TX_ENABLED, CONFIG_ESP_WIFI_AMPDU_RX_ENABLED);
        lv_label_set_text(lbl, body);
    }
    lvgl_port_unlock();
    ESP_LOGE(TAG, "boot error (%s): %s", stage, esp_err_to_name(err));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    dashcdg_log_startup_runtime_cfg();

    {
        esp_err_t w = dashcdg_wifi_boot_auto_connect();
        if (w != ESP_OK && w != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Wi-Fi saved creds connect: %s", esp_err_to_name(w));
        }
    }

    lv_disp_t *disp = NULL;
    err = dashcdg_display_lvgl_init(&disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "display OK");

    if (!dashcdg_touch_cal_store_has_valid()) {
        esp_err_t c = dashcdg_touch_cal_ui_present(disp, false, dashcdg_nav_home, NULL);
        if (c != ESP_OK) {
            ESP_LOGW(TAG, "touch cal UI failed (%s), opening home", esp_err_to_name(c));
            err = dashcdg_home_ui_present(disp);
            if (err != ESP_OK) {
                dashcdg_show_boot_error_screen(disp, "home_ui_present", err);
                return;
            }
        }
    } else {
        err = dashcdg_home_ui_present(disp);
        if (err != ESP_OK) {
            dashcdg_show_boot_error_screen(disp, "home_ui_present", err);
            return;
        }
    }

    /* After LVGL + touch UI are up: vbat only touches GPIO34 / ADC1_CH6 (never GPIO33 / TP_CS). */
    {
        esp_err_t vb = dashcdg_vbat_sense_init();
        if (vb != ESP_OK) {
            ESP_LOGW(TAG, "Vbat sense init: %s", esp_err_to_name(vb));
        }
    }

    ESP_LOGI(TAG, "launcher running");
}
