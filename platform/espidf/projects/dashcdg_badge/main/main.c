/*
 * dashcdg_badge - CYD 3.2" LVGL + touch launcher (Wi-Fi, Karaoke stubs; CD-G multicast later).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "badge_exec.h"
#include "badge_stats.h"
#include "audio_mgr.h"
#include "sfx_touch.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "touch_cal_store.h"
#include "touch_cal_ui.h"
#include "platform_hw.h"
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
    bool nvs_recovered = false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        nvs_recovered = true;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    dashcdg_log_startup_runtime_cfg();

    /*
     * Bring the executive layer up as early as we have NVS + event loop + esp_timer. Subsequent
     * subsystems publish boot facts and health transitions through this layer instead of relying
     * on log-message archaeology.
     */
    {
        esp_err_t ex = dashcdg_badge_exec_init();
        if (ex != ESP_OK) {
            ESP_LOGW(TAG, "badge_exec init: %s (continuing without exec layer)",
                     esp_err_to_name(ex));
        } else {
            uint32_t bits = DASHCDG_BADGE_EXEC_BOOT_NETIF_OK |
                            DASHCDG_BADGE_EXEC_BOOT_EVT_LOOP_OK |
                            (nvs_recovered ? DASHCDG_BADGE_EXEC_BOOT_NVS_RECOVERED
                                           : DASHCDG_BADGE_EXEC_BOOT_NVS_OK);
            (void)dashcdg_badge_exec_publish_boot_event(bits,
                    nvs_recovered ? "nvs_erase_reinit" : "nvs_first_boot_ok");
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_NVS,
                                                DASHCDG_BADGE_EXEC_HEALTH_OK,
                                                nvs_recovered ? "recovered" : "ok");
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_NETIF,
                                                DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_EVENT_LOOP,
                                                DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
        }
    }

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
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_DISPLAY_FATAL,
                                                   esp_err_to_name(err));
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_DISPLAY,
                                            DASHCDG_BADGE_EXEC_HEALTH_FAILED_FATAL,
                                            esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "display OK");
    (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_DISPLAY_OK, NULL);
    (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_DISPLAY,
                                        DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);

    {
        esp_err_t vb = dashcdg_vbat_sense_init();
        if (vb != ESP_OK) {
            ESP_LOGW(TAG, "Vbat sense init: %s", esp_err_to_name(vb));
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_VBAT,
                                                DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
                                                esp_err_to_name(vb));
        } else {
            (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_VBAT_OK, NULL);
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_VBAT,
                                                DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
        }
    }
    {
        esp_err_t ph = dashcdg_platform_hw_init();
        if (ph != ESP_OK) {
            ESP_LOGW(TAG, "platform hw init: %s", esp_err_to_name(ph));
            (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_HW_PARTIAL,
                                                       esp_err_to_name(ph));
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_PLATFORM_HW,
                                                DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
                                                esp_err_to_name(ph));
        } else {
            (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_HW_OK, NULL);
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_PLATFORM_HW,
                                                DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
        }
    }
    {
        esp_err_t st = dashcdg_badge_stats_init();
        if (st != ESP_OK) {
            ESP_LOGW(TAG, "badge_stats init: %s (telemetry offloaded task disabled)", esp_err_to_name(st));
        } else {
            dashcdg_badge_stats_kick();
        }
    }
    {
        esp_err_t am = dashcdg_audio_mgr_init();
        if (am != ESP_OK) {
            ESP_LOGW(TAG, "audio_mgr init: %s (RX may block on DAC path)", esp_err_to_name(am));
        }
    }
    dashcdg_sfx_touch_init();

    const bool touch_cal_valid = dashcdg_touch_cal_store_has_valid();
    if (!touch_cal_valid) {
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_TOUCH_CAL_REQUIRED,
                                                   "no_valid_calibration");
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_TOUCH,
                                            DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
                                            "cal_required");
        esp_err_t c = dashcdg_touch_cal_ui_present(disp, false, dashcdg_nav_home, NULL);
        if (c != ESP_OK) {
            ESP_LOGW(TAG, "touch cal UI failed (%s), opening home", esp_err_to_name(c));
            err = dashcdg_home_ui_present(disp);
            if (err != ESP_OK) {
                dashcdg_show_boot_error_screen(disp, "home_ui_present", err);
                (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_LVGL_UI,
                                                    DASHCDG_BADGE_EXEC_HEALTH_FAILED_FATAL,
                                                    esp_err_to_name(err));
                return;
            }
        }
    } else {
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_TOUCH_OK, NULL);
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_TOUCH,
                                            DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
        err = dashcdg_home_ui_present(disp);
        if (err != ESP_OK) {
            dashcdg_show_boot_error_screen(disp, "home_ui_present", err);
            (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_LVGL_UI,
                                                DASHCDG_BADGE_EXEC_HEALTH_FAILED_FATAL,
                                                esp_err_to_name(err));
            return;
        }
    }

    (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_LVGL_UI,
                                        DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);

    /*
     * Boot orchestrator decision. We are now interactive (home or touch-cal UI is up). DHCP may
     * still be in flight - that becomes a runtime health transition rather than a boot block.
     */
    {
        esp_err_t dec = dashcdg_badge_exec_decide_boot_complete();
        if (dec != ESP_OK && dec != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "boot decision: %s", esp_err_to_name(dec));
        }
    }

    /*
     * T7: start the periodic liveness sweep now that all primary tasks are registered. Per the
     * executive refactor spec the sweep starts in observe-only mode (logs stalls; never restarts).
     * Enabling enforce-mode is a Kconfig switch flipped after a clean soak run.
     */
    {
        esp_err_t ls = dashcdg_badge_exec_liveness_start();
        if (ls != ESP_OK && ls != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "liveness sweep start: %s", esp_err_to_name(ls));
        }
    }

    ESP_LOGI(TAG, "launcher running");
}
