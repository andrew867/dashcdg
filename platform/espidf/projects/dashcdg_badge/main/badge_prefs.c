#include "badge_prefs.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "badge_prefs";
static const char *NVS_NS = "dashcfg";
static const char *KEY_BL_OK = "bl_ok";
static const char *KEY_BL_PCT = "bl_pct";
static const char *KEY_RGB_OK = "rgb_ok";
static const char *KEY_RGB_ON = "rgb_on";
static const char *KEY_RGB_PCT = "rgb_pct";
static const char *KEY_ASLP_OK = "aslp_ok";
static const char *KEY_ASLP_ON = "aslp_on";
static const char *KEY_BEEP_OK = "beep_ok";
static const char *KEY_BEEP_PCT = "beep_pct";
static const char *KEY_TB_OK = "tb_ok";
static const char *KEY_TB_ON = "tb_on";
static const char *KEY_KVD_OK = "kvd_ok";
static const char *KEY_KVD_ON = "kvd_on";
static const char *KEY_KAD_OK = "kad_ok";
static const char *KEY_KAD_ON = "kad_on";
static const char *KEY_KRN_OK = "krn_ok";
static const char *KEY_KRN_ON = "krn_on";
static const char *KEY_KST_OK = "kst_ok";
static const char *KEY_KST_ON = "kst_on";

esp_err_t dashcdg_badge_prefs_load_brightness(uint8_t *out_pct_5_100)
{
    ESP_RETURN_ON_FALSE(out_pct_5_100 != NULL, ESP_ERR_INVALID_ARG, TAG, "out");

    *out_pct_5_100 = 100;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_BL_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 100;
    (void)nvs_get_u8(h, KEY_BL_PCT, &v);
    nvs_close(h);
    if (v < 5U) {
        v = 5U;
    }
    if (v > 100U) {
        v = 100U;
    }
    *out_pct_5_100 = v;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_brightness(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_BL_PCT, pct_5_100);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_BL_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "brightness save: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t dashcdg_badge_prefs_load_rgb_status(uint8_t *out_on, uint8_t *out_pct_5_100)
{
    ESP_RETURN_ON_FALSE(out_on != NULL && out_pct_5_100 != NULL, ESP_ERR_INVALID_ARG, TAG, "out");

    *out_on = 1;
    *out_pct_5_100 = 100;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_RGB_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t on = 1;
    (void)nvs_get_u8(h, KEY_RGB_ON, &on);
    uint8_t p = 100;
    (void)nvs_get_u8(h, KEY_RGB_PCT, &p);
    nvs_close(h);
    *out_on = (on != 0) ? 1U : 0U;
    if (p < 5U) {
        p = 5U;
    }
    if (p > 100U) {
        p = 100U;
    }
    *out_pct_5_100 = p;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_rgb_status(uint8_t on, uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW rgb: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_RGB_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_RGB_PCT, pct_5_100);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_RGB_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_auto_sleep(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_ASLP_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 1;
    (void)nvs_get_u8(h, KEY_ASLP_ON, &v);
    nvs_close(h);
    *out_on = (v != 0) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_auto_sleep(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW aslp: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_ASLP_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_ASLP_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_beep_volume(uint8_t *out_pct_5_100)
{
    ESP_RETURN_ON_FALSE(out_pct_5_100 != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_pct_5_100 = 85;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_BEEP_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 85;
    (void)nvs_get_u8(h, KEY_BEEP_PCT, &v);
    nvs_close(h);
    if (v < 5U) {
        v = 5U;
    }
    if (v > 100U) {
        v = 100U;
    }
    *out_pct_5_100 = v;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_beep_volume(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW beep: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_BEEP_PCT, pct_5_100);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_BEEP_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_touch_beep(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_TB_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 1;
    (void)nvs_get_u8(h, KEY_TB_ON, &v);
    nvs_close(h);
    *out_on = (v != 0) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_touch_beep(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW tb: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_TB_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_TB_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_karaoke_video_decode(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_KVD_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 1;
    (void)nvs_get_u8(h, KEY_KVD_ON, &v);
    nvs_close(h);
    *out_on = (v != 0) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_video_decode(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW kvd: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KVD_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KVD_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_karaoke_audio_decode(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_KAD_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 1;
    (void)nvs_get_u8(h, KEY_KAD_ON, &v);
    nvs_close(h);
    *out_on = (v != 0) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_audio_decode(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW kad: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KAD_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KAD_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_karaoke_repair_nack(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_KRN_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    {
        uint8_t v = 0U;
        err = nvs_get_u8(h, KEY_KRN_ON, &v);
        nvs_close(h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            *out_on = 1U;
            return ESP_OK;
        }
        if (err != ESP_OK) {
            *out_on = 0U;
            return ESP_OK;
        }
        *out_on = (v != 0U) ? 1U : 0U;
    }
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_repair_nack(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW krn: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KRN_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KRN_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_karaoke_v4_stats_tx(uint8_t *out_on)
{
    ESP_RETURN_ON_FALSE(out_on != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_on = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_KST_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 1;
    (void)nvs_get_u8(h, KEY_KST_ON, &v);
    nvs_close(h);
    *out_on = (v != 0) ? 1U : 0U;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_v4_stats_tx(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW kst: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KST_ON, (on != 0) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KST_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
