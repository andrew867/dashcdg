/*
 * Vbat on GPIO34: ADC1 oneshot + optional line fitting, divider math from board_cyd_freenove_32.h.
 *
 * ESP32 (classic): ADC1_CH6 == GPIO34 only. Do NOT use ADC1_CH5 here - that muxes GPIO33, which
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
#include <stdlib.h>

static const char *TAG = "vbat_sense";

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static bool s_ready;
#define VBAT_TRIM_MAX_KNOTS 5

/** Piecewise trim on divider linear pack estimate (mV): 0 = off, 2..VBAT_TRIM_MAX_KNOTS sorted knots. */
static int s_trim_n;
static int s_trim_l[VBAT_TRIM_MAX_KNOTS];
static int s_trim_mv[VBAT_TRIM_MAX_KNOTS];

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

/** Pack mV from raw using divider only (same as dashcdg_vbat_sense_read before trim). */
static int vbat_linear_mv_from_raw(int raw)
{
    int pin = pin_mv_from_raw(raw);
    if (pin < 0) {
        pin = 0;
    }
    int64_t numer = (int64_t)pin * (int64_t)(CYD_VBAT_R_OHM_TOP + CYD_VBAT_R_OHM_BOTTOM);
    return (int)(numer / (int64_t)CYD_VBAT_R_OHM_BOTTOM);
}

static void vbat_trim_sort_knots(int n, int *l, int *mv)
{
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (l[j] < l[i]) {
                int t = l[i];
                l[i] = l[j];
                l[j] = t;
                t = mv[i];
                mv[i] = mv[j];
                mv[j] = t;
            }
        }
    }
}

/** Map divider-linear pack mV -> trimmed pack mV (extrapolates beyond end knots). */
static int vbat_apply_trim(int vbat_lin)
{
    if (s_trim_n < 2) {
        return vbat_lin;
    }
    int n = s_trim_n;
    int i = 0;
    while (i < n - 2 && vbat_lin > s_trim_l[i + 1]) {
        i++;
    }
    int64_t dmv = (int64_t)s_trim_mv[i + 1] - (int64_t)s_trim_mv[i];
    int64_t dl = (int64_t)s_trim_l[i + 1] - (int64_t)s_trim_l[i];
    if (dl == 0) {
        return s_trim_mv[i];
    }
    return (int)(s_trim_mv[i] + dmv * ((int64_t)vbat_lin - (int64_t)s_trim_l[i]) / dl);
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

    s_trim_n = 0;
#if DASHCDG_VBAT_CAL_POINT1_RAW > 0 && DASHCDG_VBAT_CAL_POINT0_RAW > 0
    {
        int l[VBAT_TRIM_MAX_KNOTS];
        int mv[VBAT_TRIM_MAX_KNOTS];
        int n = 0;
        l[n] = vbat_linear_mv_from_raw(DASHCDG_VBAT_CAL_POINT0_RAW);
        mv[n] = DASHCDG_VBAT_CAL_POINT0_MV;
        n++;
        l[n] = vbat_linear_mv_from_raw(DASHCDG_VBAT_CAL_POINT1_RAW);
        mv[n] = DASHCDG_VBAT_CAL_POINT1_MV;
        n++;
#if DASHCDG_VBAT_CAL_POINT2_RAW > 0
        l[n] = vbat_linear_mv_from_raw(DASHCDG_VBAT_CAL_POINT2_RAW);
        mv[n] = DASHCDG_VBAT_CAL_POINT2_MV;
        n++;
#if DASHCDG_VBAT_CAL_POINT3_RAW > 0
        l[n] = vbat_linear_mv_from_raw(DASHCDG_VBAT_CAL_POINT3_RAW);
        mv[n] = DASHCDG_VBAT_CAL_POINT3_MV;
        n++;
#if DASHCDG_VBAT_CAL_POINT4_RAW > 0
        l[n] = vbat_linear_mv_from_raw(DASHCDG_VBAT_CAL_POINT4_RAW);
        mv[n] = DASHCDG_VBAT_CAL_POINT4_MV;
        n++;
#endif
#endif
#endif
        vbat_trim_sort_knots(n, l, mv);
        bool ok = true;
        for (int i = 0; i < n; i++) {
            if (l[i] < 100) {
                ok = false;
            }
        }
        for (int i = 0; i < n - 1; i++) {
            if (l[i + 1] - l[i] < 5) {
                ok = false;
            }
        }
        if (ok) {
            for (int i = 0; i < n; i++) {
                s_trim_l[i] = l[i];
                s_trim_mv[i] = mv[i];
            }
            s_trim_n = n;
            if (n == 2) {
                ESP_LOGI(TAG,
                         "VBAT 2-pt cal (sorted knots): est %d mV -> true %d; est %d mV -> true %d (raw in %d,%d)",
                         l[0], mv[0], l[1], mv[1], DASHCDG_VBAT_CAL_POINT0_RAW, DASHCDG_VBAT_CAL_POINT1_RAW);
            } else if (n == 3) {
                ESP_LOGI(TAG,
                         "VBAT 3-pt cal: knots est %d/%d/%d mV -> true %d/%d/%d mV (raw ref %d,%d,%d)",
                         l[0], l[1], l[2], mv[0], mv[1], mv[2], DASHCDG_VBAT_CAL_POINT0_RAW,
                         DASHCDG_VBAT_CAL_POINT1_RAW, DASHCDG_VBAT_CAL_POINT2_RAW);
            } else if (n == 4) {
                ESP_LOGI(TAG,
                         "VBAT 4-pt cal: knots est %d/%d/%d/%d mV -> true %d/%d/%d/%d mV (raw ref %d,%d,%d,%d)",
                         l[0], l[1], l[2], l[3], mv[0], mv[1], mv[2], mv[3], DASHCDG_VBAT_CAL_POINT0_RAW,
                         DASHCDG_VBAT_CAL_POINT1_RAW, DASHCDG_VBAT_CAL_POINT2_RAW, DASHCDG_VBAT_CAL_POINT3_RAW);
            } else {
                ESP_LOGI(TAG,
                         "VBAT 5-pt cal: knots est %d/%d/%d/%d/%d mV -> true %d/%d/%d/%d/%d mV (raw ref "
                         "%d,%d,%d,%d,%d)",
                         l[0], l[1], l[2], l[3], l[4], mv[0], mv[1], mv[2], mv[3], mv[4],
                         DASHCDG_VBAT_CAL_POINT0_RAW, DASHCDG_VBAT_CAL_POINT1_RAW, DASHCDG_VBAT_CAL_POINT2_RAW,
                         DASHCDG_VBAT_CAL_POINT3_RAW, DASHCDG_VBAT_CAL_POINT4_RAW);
            }
        } else {
            ESP_LOGW(TAG, "VBAT cal disabled (n=%d l=%d,%d,%d,%d,%d)", n, l[0], n > 1 ? l[1] : 0, n > 2 ? l[2] : 0,
                     n > 3 ? l[3] : 0, n > 4 ? l[4] : 0);
        }
    }
#endif

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
    if (s_trim_n >= 2) {
        vbat = vbat_apply_trim(vbat);
        if (vbat < 2500) {
            vbat = 2500;
        }
        if (vbat > 4500) {
            vbat = 4500;
        }
    }

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
