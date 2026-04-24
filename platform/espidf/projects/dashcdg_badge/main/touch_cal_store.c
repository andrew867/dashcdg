/*
 * Persist XPT2046 linear ADC bounds (same role as TFT_eSPI setTouch(calData)[0..3]).
 */
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "touch_cal_store.h"

static const char *TAG = "touch_cal_nvs";
static const char *NVS_NS = "dashcfg";

#define KEY_VALID "tcal_ok"
#define KEY_X0 "tcal_x0"
#define KEY_X1 "tcal_x1"
#define KEY_Y0 "tcal_y0"
#define KEY_Y1 "tcal_y1"

bool dashcdg_touch_cal_store_has_valid(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, KEY_VALID, &v);
    nvs_close(h);
    return (err == ESP_OK && v == 1);
}

esp_err_t dashcdg_touch_cal_store_load(uint16_t *x_min, uint16_t *x_max, uint16_t *y_min, uint16_t *y_max)
{
    ESP_RETURN_ON_FALSE(x_min && x_max && y_min && y_max, ESP_ERR_INVALID_ARG, TAG, "args");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t ok = 0;
    err = nvs_get_u8(h, KEY_VALID, &ok);
    if (err != ESP_OK || ok != 1) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t v;
    err = nvs_get_u16(h, KEY_X0, &v);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    *x_min = v;

    err = nvs_get_u16(h, KEY_X1, &v);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    *x_max = v;

    err = nvs_get_u16(h, KEY_Y0, &v);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    *y_min = v;

    err = nvs_get_u16(h, KEY_Y1, &v);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    *y_max = v;

    return ESP_OK;
}

esp_err_t dashcdg_touch_cal_store_save(uint16_t x_min, uint16_t x_max, uint16_t y_min, uint16_t y_max)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");

    ESP_RETURN_ON_ERROR(nvs_set_u16(h, KEY_X0, x_min), TAG, "x0");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, KEY_X1, x_max), TAG, "x1");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, KEY_Y0, y_min), TAG, "y0");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, KEY_Y1, y_max), TAG, "y1");
    ESP_RETURN_ON_ERROR(nvs_set_u8(h, KEY_VALID, 1), TAG, "ok");

    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t dashcdg_touch_cal_store_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_VALID, 0);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
