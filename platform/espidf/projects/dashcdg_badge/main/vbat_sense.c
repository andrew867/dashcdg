/*
 * Vbat on GPIO34: ADC1 oneshot + optional line fitting, divider math from board_cyd_freenove_32.h.
 *
 * ESP32 (classic): ADC1_CH6 == GPIO34 only. Do NOT use ADC1_CH5 here — that muxes GPIO33, which
 * is XPT2046 TP_CS on the CYD; reassigning it to SARADC kills SPI touch chip-select.
 */
#include "vbat_sense.h"

#include "board_cyd_freenove_32.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "hal/adc_types.h"

#include <stdio.h>

static const char *TAG = "vbat_sense";

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static bool s_ready;

/* ESP32: GPIO34 == ADC1 ch6. */
#define VBAT_ADC_UNIT     ADC_UNIT_1
#define VBAT_ADC_CH       ADC_CHANNEL_6
#define VBAT_OVERSAMPLE_N 12

static esp_err_t vbat_sense_install_cali(void)
{
    s_cali_ok = false;
#if CONFIG_IDF_TARGET_ESP32
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = VBAT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 0,
    };
    if (adc_cali_create_scheme_line_fitting(&cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    }
#endif
    return ESP_OK;
}

static int pin_mv_from_raw(int raw)
{
    int mV;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mV) != ESP_OK) {
            mV = 0;
        }
    } else {
        mV = (int)((int64_t)raw * 3300 / 4095);
    }
    return mV;
}

esp_err_t dashcdg_vbat_sense_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_unit = NULL;
    s_cali = NULL;
    s_cali_ok = false;

    adc_oneshot_unit_init_cfg_t u = {
        .unit_id = VBAT_ADC_UNIT,
    };
    esp_err_t e = adc_oneshot_new_unit(&u, &s_unit);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(e));
        return e;
    }
    adc_oneshot_chan_cfg_t ch = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    e = adc_oneshot_config_channel(s_unit, VBAT_ADC_CH, &ch);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(e));
        adc_oneshot_del_unit(s_unit);
        s_unit = NULL;
        return e;
    }

    vbat_sense_install_cali();

    s_ready = true;
    ESP_LOGI(TAG, "Vbat sense ready (IO%u, cali %s)", (unsigned)CYD_GPIO_VBAT_SENSE, s_cali_ok ? "on" : "off");
    return ESP_OK;
}

bool dashcdg_vbat_sense_is_ready(void)
{
    return s_ready && s_unit != NULL;
}

esp_err_t dashcdg_vbat_sense_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv)
{
    if (!s_ready || !s_unit) {
        return ESP_ERR_INVALID_STATE;
    }
    int acc = 0;
    for (int i = 0; i < VBAT_OVERSAMPLE_N; i++) {
        int v = 0;
        if (adc_oneshot_read(s_unit, VBAT_ADC_CH, &v) != ESP_OK) {
            v = 0;
        }
        acc += v;
    }
    int raw = (acc + VBAT_OVERSAMPLE_N / 2) / VBAT_OVERSAMPLE_N;

    int pin = pin_mv_from_raw(raw);
    int64_t numer = (int64_t)pin * (int64_t)(CYD_VBAT_R_OHM_TOP + CYD_VBAT_R_OHM_BOTTOM);
    int vbat = (int)(numer / (int64_t)CYD_VBAT_R_OHM_BOTTOM);

    if (out_raw) {
        *out_raw = raw;
    }
    if (out_pin_mv) {
        *out_pin_mv = pin;
    }
    if (out_vbat_mv) {
        *out_vbat_mv = vbat;
    }
    return ESP_OK;
}

void dashcdg_vbat_sense_format_line(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz < 4) {
        return;
    }
    if (!dashcdg_vbat_sense_is_ready()) {
        snprintf(buf, buf_sz, " -- ");
        return;
    }
    int raw = 0, vbat = 0;
    if (dashcdg_vbat_sense_read(&raw, NULL, &vbat) != ESP_OK) {
        snprintf(buf, buf_sz, "err");
        return;
    }
    int deci = (vbat * 10 + 500) / 1000;
    if (deci < 0) {
        deci = 0;
    }
    int w = deci / 10;
    int f = deci % 10;
    snprintf(buf, buf_sz, "%4d %d.%dV", raw, w, f);
}
