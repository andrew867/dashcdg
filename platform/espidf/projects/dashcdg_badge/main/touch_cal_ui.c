/*
 * Four-corner XPT2046 calibration; saves linear ADC bounds to NVS (see touch_cal_store).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "misc/lv_async.h"

#include "board_cyd_freenove_32.h"
#include "display_lvgl.h"
#include "touch_cal_store.h"
#include "touch_cal_ui.h"

static const char *TAG = "touch_cal_ui";

#define SAMPLE_TARGET 8
/** Min raw ADC span after extrapolation (relaxed; clamp can shrink edge extrapolation). */
#define MIN_SPAN_EXTRUDED 72
/** Fallback: min/max of corner samples without extrapolation (usually wider span). */
#define MIN_SPAN_LEGACY 48
/** Consecutive timer ticks (40ms each) with pen up before accepting release. */
#define RELEASE_STABLE_TICKS 6
/** Target size (px); placed flush to each display corner. */
#define CROSS_SZ 20

typedef enum {
    CAL_IDLE,
    CAL_SAMPLING,
    CAL_WAIT_RELEASE,
} cal_phase_t;

/** Visual state for the corner target (idle / pen down / release). */
typedef enum {
    CROSS_VIS_IDLE = 0,
    CROSS_VIS_PEN_DOWN,
    CROSS_VIS_RELEASE,
} cross_vis_t;

typedef struct {
    lv_disp_t *disp;
    lv_obj_t *lbl_instr;
    lv_obj_t *lbl_debug;
    lv_obj_t *cross;
    lv_obj_t *cross_lbl;
    lv_timer_t *timer;
    uint8_t step;
    cal_phase_t phase;
    uint32_t sum_x;
    uint32_t sum_y;
    uint8_t n_samples;
    uint16_t corner_rx[4];
    uint16_t corner_ry[4];
    bool show_cancel;
    uint8_t release_stable;
    dashcdg_touch_cal_nav_fn on_done;
    dashcdg_touch_cal_nav_fn on_cancel;
} cal_ctx_t;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static const char *k_step_text[4] = {
    "Tap the cross: top-left",
    "Tap the cross: top-right",
    "Tap the cross: bottom-right",
    "Tap the cross: bottom-left",
};

static void cal_timer_cb(lv_timer_t *t);

static void cal_position_cross(cal_ctx_t *c, uint8_t step)
{
    if (!c || !c->cross || step > 3) {
        return;
    }
    const lv_coord_t sz = CROSS_SZ;
    const lv_coord_t w = CYD_LCD_H_RES;
    const lv_coord_t h = CYD_LCD_V_RES;
    const lv_coord_t x[4] = {0, w - sz, w - sz, 0};
    const lv_coord_t y[4] = {0, 0, h - sz, h - sz};
    lv_obj_set_pos(c->cross, x[step], y[step]);
}

static void cal_cross_visual(cal_ctx_t *c, cross_vis_t vis)
{
    if (!c || !c->cross) {
        return;
    }
    switch (vis) {
    case CROSS_VIS_IDLE:
        lv_obj_set_style_bg_color(c->cross, lv_color_hex(0x113322), 0);
        lv_obj_set_style_border_color(c->cross, lv_color_hex(0x00ff88), 0);
        if (c->cross_lbl) {
            lv_obj_set_style_text_color(c->cross_lbl, lv_color_hex(0x66ffaa), 0);
        }
        break;
    case CROSS_VIS_PEN_DOWN:
        lv_obj_set_style_bg_color(c->cross, lv_color_hex(0x331028), 0);
        lv_obj_set_style_border_color(c->cross, lv_color_hex(0xff44dd), 0);
        if (c->cross_lbl) {
            lv_obj_set_style_text_color(c->cross_lbl, lv_color_hex(0xffaaee), 0);
        }
        break;
    case CROSS_VIS_RELEASE:
        lv_obj_set_style_bg_color(c->cross, lv_color_hex(0x332208), 0);
        lv_obj_set_style_border_color(c->cross, lv_color_hex(0xffcc00), 0);
        if (c->cross_lbl) {
            lv_obj_set_style_text_color(c->cross_lbl, lv_color_hex(0xffee88), 0);
        }
        break;
    }
}

static void cal_destroy(cal_ctx_t *c)
{
    if (!c) {
        return;
    }
    if (c->timer) {
        lv_timer_del(c->timer);
        c->timer = NULL;
    }
    dashcdg_touch_rearm_locked();
    free(c);
}

static void cal_fail(cal_ctx_t *c, const char *msg)
{
    if (c->lbl_debug) {
        lv_label_set_text(c->lbl_debug, msg);
    }
    /* cal_complete_ok runs with step==4 after the last corner; rewind for a clean retry. */
    if (c->step >= 4) {
        c->step = 0;
        cal_position_cross(c, 0);
    }
    if (c->lbl_instr) {
        lv_label_set_text(c->lbl_instr, k_step_text[c->step]);
    }
    ESP_LOGW(TAG, "%s", msg);
    c->phase = CAL_IDLE;
    c->n_samples = 0;
    c->sum_x = 0;
    c->sum_y = 0;
    c->release_stable = 0;
    cal_cross_visual(c, CROSS_VIS_IDLE);
    /* cal_complete_ok may have stopped the timer; restart so user can retry. */
    if (c->timer == NULL && c->disp) {
        c->timer = lv_timer_create(cal_timer_cb, 40, c);
    }
}

/**
 * Raw samples at cross centers (r = CROSS_SZ/2). NVS stores chip X register bounds in xmin/xmax
 * and chip Y register in ymin/ymax (same as TFT_eSPI calData). display_lvgl maps chip Y → LVGL X
 * and chip X → LVGL Y, so extrapolation pairs axes to **screen** geometry as follows:
 *   - adc Y vs horizontal position: left column (TL,BL) vs right (TR,BR)
 *   - adc X vs vertical position: top row (TL,TR) vs bottom (BL,BR)
 */
static bool cal_extrude_edges_to_adc_range(const uint16_t corner_rx[4], const uint16_t corner_ry[4],
                                           uint16_t *out_xmin, uint16_t *out_xmax, uint16_t *out_ymin,
                                           uint16_t *out_ymax)
{
    const int32_t W = CYD_LCD_H_RES;
    const int32_t H = CYD_LCD_V_RES;
    const int32_t r = CROSS_SZ / 2;

    /* Chip Y-ADC vs LVGL X: left vs right screen columns */
    int32_t raw_yl = ((int32_t)corner_ry[0] + (int32_t)corner_ry[3]) / 2;
    int32_t raw_yr = ((int32_t)corner_ry[1] + (int32_t)corner_ry[2]) / 2;
    int32_t sx_l = r;
    int32_t sx_r = W - r;
    int32_t span_x = sx_r - sx_l;
    if (span_x <= 0) {
        return false;
    }

    int32_t ymin_i = raw_yl + (raw_yr - raw_yl) * (0 - sx_l) / span_x;
    int32_t ymax_i = raw_yl + (raw_yr - raw_yl) * ((W - 1) - sx_l) / span_x;

    /* Chip X-ADC vs LVGL Y: top vs bottom screen rows */
    int32_t raw_xt = ((int32_t)corner_rx[0] + (int32_t)corner_rx[1]) / 2;
    int32_t raw_xb = ((int32_t)corner_rx[3] + (int32_t)corner_rx[2]) / 2;
    int32_t sy_t = r;
    int32_t sy_b = H - r;
    int32_t span_y = sy_b - sy_t;
    if (span_y <= 0) {
        return false;
    }

    int32_t xmin_i = raw_xt + (raw_xb - raw_xt) * (0 - sy_t) / span_y;
    int32_t xmax_i = raw_xt + (raw_xb - raw_xt) * ((H - 1) - sy_t) / span_y;

    if (xmin_i > xmax_i) {
        int32_t t = xmin_i;
        xmin_i = xmax_i;
        xmax_i = t;
    }
    if (ymin_i > ymax_i) {
        int32_t t = ymin_i;
        ymin_i = ymax_i;
        ymax_i = t;
    }

    const int32_t adc_max = 4095;
    xmin_i = (xmin_i < 0) ? 0 : ((xmin_i > adc_max) ? adc_max : xmin_i);
    xmax_i = (xmax_i < 0) ? 0 : ((xmax_i > adc_max) ? adc_max : xmax_i);
    ymin_i = (ymin_i < 0) ? 0 : ((ymin_i > adc_max) ? adc_max : ymin_i);
    ymax_i = (ymax_i < 0) ? 0 : ((ymax_i > adc_max) ? adc_max : ymax_i);

    *out_xmin = (uint16_t)xmin_i;
    *out_xmax = (uint16_t)xmax_i;
    *out_ymin = (uint16_t)ymin_i;
    *out_ymax = (uint16_t)ymax_i;
    return true;
}

/** Corner box without extrapolation; axis pairing matches cal_extrude_edges_to_adc_range(). */
static void cal_legacy_corner_minmax(const uint16_t crx[4], const uint16_t cry[4], uint16_t *xmin,
                                      uint16_t *xmax, uint16_t *ymin, uint16_t *ymax)
{
    *xmin = MIN(crx[0], crx[1]);
    *xmax = MAX(crx[3], crx[2]);
    *ymin = MIN(cry[0], cry[3]);
    *ymax = MAX(cry[1], cry[2]);
    if (*xmin > *xmax) {
        uint16_t t = *xmin;
        *xmin = *xmax;
        *xmax = t;
    }
    if (*ymin > *ymax) {
        uint16_t t = *ymin;
        *ymin = *ymax;
        *ymax = t;
    }
}

typedef struct {
    lv_disp_t *disp;
    dashcdg_touch_cal_nav_fn fn;
} cal_nav_defer_t;

/** Run navigation after the current LVGL timer/event stack unwinds (avoids lvgl_port_lock deadlock). */
static void cal_nav_deferred_body(void *ud)
{
    cal_nav_defer_t *p = (cal_nav_defer_t *)ud;
    if (p && p->fn && p->disp) {
        p->fn(p->disp);
    }
    free(p);
}

static void cal_nav_dispatch_async(lv_disp_t *disp, dashcdg_touch_cal_nav_fn fn)
{
    if (!fn || !disp) {
        return;
    }
    cal_nav_defer_t *p = (cal_nav_defer_t *)calloc(1, sizeof(cal_nav_defer_t));
    if (!p) {
        fn(disp);
        return;
    }
    p->disp = disp;
    p->fn = fn;
    if (lv_async_call(cal_nav_deferred_body, p) != LV_RESULT_OK) {
        fn(disp);
        free(p);
    }
}

static void cal_complete_ok(cal_ctx_t *c)
{
    uint16_t xmin = 0;
    uint16_t xmax = 0;
    uint16_t ymin = 0;
    uint16_t ymax = 0;

    uint16_t xe = 0;
    uint16_t xeM = 0;
    uint16_t ye = 0;
    uint16_t yeM = 0;
    const bool have_extr = cal_extrude_edges_to_adc_range(c->corner_rx, c->corner_ry, &xe, &xeM, &ye, &yeM);

    uint16_t xl = 0;
    uint16_t xlM = 0;
    uint16_t yl = 0;
    uint16_t ylM = 0;
    cal_legacy_corner_minmax(c->corner_rx, c->corner_ry, &xl, &xlM, &yl, &ylM);

    const bool extr_ok = have_extr && ((uint32_t)xeM > (uint32_t)xe + MIN_SPAN_EXTRUDED) &&
                         ((uint32_t)yeM > (uint32_t)ye + MIN_SPAN_EXTRUDED);
    const bool leg_ok = ((uint32_t)xlM > (uint32_t)xl + MIN_SPAN_LEGACY) &&
                        ((uint32_t)ylM > (uint32_t)yl + MIN_SPAN_LEGACY);

    if (extr_ok) {
        xmin = xe;
        xmax = xeM;
        ymin = ye;
        ymax = yeM;
    } else if (leg_ok) {
        xmin = xl;
        xmax = xlM;
        ymin = yl;
        ymax = ylM;
        ESP_LOGI(TAG, "touch cal: using legacy min/max (extruded %s)", have_extr ? "span tight" : "skipped");
    } else {
        cal_fail(c, "Range too small - retry corners");
        return;
    }

    esp_err_t e = dashcdg_touch_cal_store_save(xmin, xmax, ymin, ymax);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(e));
        if (c->lbl_debug) {
            lv_label_set_text(c->lbl_debug, "NVS save failed - exit");
        }
        dashcdg_touch_cal_nav_fn done = c->on_done;
        lv_disp_t *disp = c->disp;
        if (c->timer) {
            lv_timer_del(c->timer);
            c->timer = NULL;
        }
        dashcdg_touch_rearm_locked();
        free(c);
        cal_nav_dispatch_async(disp, done);
        return;
    }
    dashcdg_touch_set_calibration_adc(xmin, xmax, ymin, ymax);
    ESP_LOGI(TAG, "touch cal saved: X %u..%u  Y %u..%u", xmin, xmax, ymin, ymax);

    dashcdg_touch_cal_nav_fn done = c->on_done;
    lv_disp_t *disp = c->disp;
    cal_destroy(c);
    cal_nav_dispatch_async(disp, done);
}

/**
 * Never call cal_complete_ok() from inside cal_timer_cb: deleting/freeing the timer's user_data
 * from its own callback breaks LVGL's timer list so lv_async_call may never run. Stop the timer
 * first, then finish on the next async invocation.
 */
static void cal_async_do_complete(void *ud)
{
    cal_ctx_t *c = (cal_ctx_t *)ud;
    if (!c) {
        return;
    }
    cal_complete_ok(c);
}

static void cal_timer_cb(lv_timer_t *t)
{
    cal_ctx_t *c = (cal_ctx_t *)lv_timer_get_user_data(t);
    if (!c || !c->disp) {
        return;
    }

    uint16_t rx = 0, ry = 0;
    bool down = false;
    if (!dashcdg_touch_read_raw_adc(&rx, &ry, &down)) {
        return;
    }

    char dbg[56];
    if (c->phase == CAL_WAIT_RELEASE) {
        snprintf(dbg, sizeof(dbg), "raw %u,%u  RELEASE", rx, ry);
    } else {
        snprintf(dbg, sizeof(dbg), "raw %u,%u %s", rx, ry, down ? "DOWN" : "up");
    }
    if (c->lbl_debug) {
        lv_label_set_text(c->lbl_debug, dbg);
    }

    if (c->step >= 4) {
        return;
    }

    switch (c->phase) {
    case CAL_IDLE:
        if (down) {
            c->phase = CAL_SAMPLING;
            c->sum_x = 0;
            c->sum_y = 0;
            c->n_samples = 0;
            cal_cross_visual(c, CROSS_VIS_PEN_DOWN);
        }
        break;

    case CAL_SAMPLING:
        if (!down) {
            c->phase = CAL_IDLE;
            c->n_samples = 0;
            c->sum_x = 0;
            c->sum_y = 0;
            cal_cross_visual(c, CROSS_VIS_IDLE);
            break;
        }
        c->sum_x += rx;
        c->sum_y += ry;
        c->n_samples++;
        if (c->n_samples >= SAMPLE_TARGET) {
            c->corner_rx[c->step] = (uint16_t)(c->sum_x / SAMPLE_TARGET);
            c->corner_ry[c->step] = (uint16_t)(c->sum_y / SAMPLE_TARGET);
            c->phase = CAL_WAIT_RELEASE;
            c->release_stable = 0;
            cal_cross_visual(c, CROSS_VIS_RELEASE);
            if (c->lbl_instr) {
                if (c->step == 3) {
                    lv_label_set_text(c->lbl_instr, "Release stylus - finish");
                } else {
                    lv_label_set_text(c->lbl_instr, "Release stylus - next corner follows");
                }
            }
        }
        break;

    case CAL_WAIT_RELEASE:
        if (!down) {
            if (c->release_stable < 250) {
                c->release_stable++;
            }
            if (c->release_stable < RELEASE_STABLE_TICKS) {
                break;
            }
            c->release_stable = 0;
            c->step++;
            c->phase = CAL_IDLE;
            cal_cross_visual(c, CROSS_VIS_IDLE);
            if (c->step < 4) {
                if (c->lbl_instr) {
                    lv_label_set_text(c->lbl_instr, k_step_text[c->step]);
                }
                cal_position_cross(c, c->step);
            } else {
                lv_timer_del(t);
                c->timer = NULL;
                if (lv_async_call(cal_async_do_complete, c) != LV_RESULT_OK) {
                    cal_async_do_complete(c);
                }
            }
        } else {
            c->release_stable = 0;
        }
        break;
    }
}

static void on_cancel_clicked(lv_event_t *e)
{
    cal_ctx_t *c = (cal_ctx_t *)lv_event_get_user_data(e);
    if (!c) {
        return;
    }
    dashcdg_touch_cal_nav_fn cancel = c->on_cancel;
    lv_disp_t *disp = c->disp;
    cal_destroy(c);
    cal_nav_dispatch_async(disp, cancel);
}

/** LVGL 9: containers default to scrollable; strip chrome from footer. */
static void ui_no_scroll(lv_obj_t *o)
{
    if (o) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
}

esp_err_t dashcdg_touch_cal_ui_present(lv_disp_t *disp, bool show_cancel_button, dashcdg_touch_cal_nav_fn on_done,
                                       dashcdg_touch_cal_nav_fn on_cancel)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");
    ESP_RETURN_ON_FALSE(on_done != NULL, ESP_ERR_INVALID_ARG, TAG, "on_done");

    cal_ctx_t *c = (cal_ctx_t *)calloc(1, sizeof(cal_ctx_t));
    ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_NO_MEM, TAG, "calloc");
    c->disp = disp;
    c->show_cancel = show_cancel_button;
    c->on_done = on_done;
    c->on_cancel = show_cancel_button ? on_cancel : NULL;

    if (!lvgl_port_lock(1000)) {
        free(c);
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_touch_input_enable(false);

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020403), 0);
    ui_no_scroll(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    /* Bottom stack: title, instruction, raw debug, cancel - keeps all four corners clear for targets. */
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_style_pad_left(foot, 8, 0);
    lv_obj_set_style_pad_right(foot, 8, 0);
    lv_obj_set_style_pad_top(foot, 10, 0);
    lv_obj_set_style_pad_bottom(foot, 8, 0);
    lv_obj_set_style_pad_row(foot, 8, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_no_scroll(foot);

    lv_obj_t *title = lv_label_create(foot);
    lv_label_set_text(title, "> touch_cal // 4-corner");
    lv_obj_set_style_text_color(title, lv_color_hex(0x33ff99), 0);

    c->lbl_instr = lv_label_create(foot);
    lv_label_set_text(c->lbl_instr, k_step_text[0]);
    lv_obj_set_style_text_color(c->lbl_instr, lv_color_hex(0x88ccb0), 0);
    lv_label_set_long_mode(c->lbl_instr, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(c->lbl_instr, 300);
    lv_obj_set_style_text_align(c->lbl_instr, LV_TEXT_ALIGN_CENTER, 0);

    c->lbl_debug = lv_label_create(foot);
    lv_label_set_text(c->lbl_debug, "--");
    lv_obj_set_style_text_color(c->lbl_debug, lv_color_hex(0x558866), 0);
    lv_obj_set_style_text_align(c->lbl_debug, LV_TEXT_ALIGN_CENTER, 0);

    if (show_cancel_button && on_cancel) {
        lv_obj_t *b = lv_button_create(foot);
        lv_obj_set_width(b, 120);
        lv_obj_set_height(b, 32);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x0a1510), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x338866), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_t *lb = lv_label_create(b);
        lv_label_set_text(lb, "Cancel");
        lv_obj_set_style_text_color(lb, lv_color_hex(0x88ffcc), 0);
        lv_obj_center(lb);
        lv_obj_add_event_cb(b, on_cancel_clicked, LV_EVENT_CLICKED, c);
    }

    /* Targets last so they paint above the footer strip (corners remain unobstructed). */
    c->cross = lv_obj_create(scr);
    lv_obj_set_size(c->cross, CROSS_SZ, CROSS_SZ);
    lv_obj_set_style_radius(c->cross, CROSS_SZ / 2, 0);
    ui_no_scroll(c->cross);
    cal_position_cross(c, 0);
    cal_cross_visual(c, CROSS_VIS_IDLE);

    c->cross_lbl = lv_label_create(c->cross);
    lv_label_set_text(c->cross_lbl, "+");
    lv_obj_center(c->cross_lbl);
    lv_obj_move_foreground(c->cross);

    c->timer = lv_timer_create(cal_timer_cb, 40, c);
    if (!c->timer) {
        dashcdg_touch_rearm_locked();
        lvgl_port_unlock();
        free(c);
        return ESP_FAIL;
    }

    lv_obj_invalidate(scr);
    lvgl_port_unlock();
    return ESP_OK;
}
