#include "badge_prefs.h"

#include <stdint.h>

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
static const char *KEY_KMP_OK = "kmp_ok";
static const char *KEY_KMP_MD = "kmp_md";
static const char *KEY_KOVOL_OK = "kov_ok";
static const char *KEY_KOVOL_PCT = "kov_pct";

static uint64_t s_kovol_save_deadline_ms;
static uint8_t s_kovol_pending_pct;

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

esp_err_t dashcdg_badge_prefs_load_karaoke_media_path_policy(uint8_t *out_mode)
{
    ESP_RETURN_ON_FALSE(out_mode != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_mode = 3U;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    {
        uint8_t ok = 0U;
        err = nvs_get_u8(h, KEY_KMP_OK, &ok);
        if (err != ESP_OK || ok != 1U) {
            nvs_close(h);
            return ESP_OK;
        }
    }
    {
        uint8_t v = 3U;
        (void)nvs_get_u8(h, KEY_KMP_MD, &v);
        nvs_close(h);
        if (v > 3U) {
            v = 3U;
        }
        *out_mode = v;
    }
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_media_path_policy(uint8_t mode)
{
    nvs_handle_t h;
    esp_err_t err;

    if (mode > 3U) {
        mode = 3U;
    }
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW kmp: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KMP_MD, mode);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KMP_OK, 1U);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dashcdg_badge_prefs_load_karaoke_output_volume(uint8_t *out_pct_0_100)
{
    ESP_RETURN_ON_FALSE(out_pct_0_100 != NULL, ESP_ERR_INVALID_ARG, TAG, "out");

    *out_pct_0_100 = 85U;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_KOVOL_OK, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_OK;
    }
    uint8_t v = 85U;
    (void)nvs_get_u8(h, KEY_KOVOL_PCT, &v);
    nvs_close(h);
    if (v > 100U) {
        v = 100U;
    }
    *out_pct_0_100 = v;
    return ESP_OK;
}

esp_err_t dashcdg_badge_prefs_save_karaoke_output_volume(uint8_t pct_0_100)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW kovol: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, KEY_KOVOL_PCT, pct_0_100);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_KOVOL_OK, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "karaoke output volume save: %s", esp_err_to_name(err));
    }
    return err;
}

void dashcdg_badge_prefs_schedule_karaoke_output_volume_save(uint8_t pct_0_100, uint64_t now_ms)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    s_kovol_pending_pct = pct_0_100;
    s_kovol_save_deadline_ms = now_ms + 5000ULL;
}

void dashcdg_badge_prefs_poll_karaoke_output_volume_save(uint64_t now_ms)
{
    if (s_kovol_save_deadline_ms == 0ULL) {
        return;
    }
    if (now_ms < s_kovol_save_deadline_ms) {
        return;
    }
    uint8_t v = s_kovol_pending_pct;
    s_kovol_save_deadline_ms = 0ULL;
    (void)dashcdg_badge_prefs_save_karaoke_output_volume(v);
}
