/*
 * v4 multicast CDG proof: full CDG viewport + panel band blit; mcast diagnostics in (i) modal.
 */
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "badge_rx.h"
#include "display_lvgl.h"
#include "karaoke_ui.h"
#include "nav.h"

static const char *TAG = "karaoke_ui";

static lv_timer_t *s_tick;
/** Modal body label; refreshed while open (NULL when closed). */
static lv_obj_t *s_mcast_modal_lbl;
static lv_obj_t *s_mcast_modal_root;
/** Layout anchor + border; pixels drawn via esp_lcd_panel_draw_bitmap (see badge_rx_ui_tick). */
static lv_obj_t *s_cdg_slot;

typedef struct {
    lv_disp_t *disp;
} tick_ctx_t;

static tick_ctx_t s_tick_ctx;
static uint32_t s_heap_retry_ticks;

static void dashcdg_ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void mcast_modal_close(void)
{
    if (s_mcast_modal_root && lv_obj_is_valid(s_mcast_modal_root)) {
        lv_obj_del(s_mcast_modal_root);
    }
    s_mcast_modal_root = NULL;
    s_mcast_modal_lbl = NULL;
}

void dashcdg_karaoke_ui_teardown(void)
{
    mcast_modal_close();
    if (lvgl_port_lock(1000)) {
        if (s_tick) {
            lv_timer_del(s_tick);
            s_tick = NULL;
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "teardown: LVGL lock timeout (timer not removed)");
    }
    s_tick_ctx.disp = NULL;
    s_cdg_slot = NULL;
}

static void on_mcast_scrim_clicked(lv_event_t *e)
{
    (void)e;
    mcast_modal_close();
}

static void on_mcast_panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void on_mcast_ok(lv_event_t *e)
{
    (void)e;
    mcast_modal_close();
}

static void on_info_btn(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    (void)disp;
    if (s_mcast_modal_root) {
        return;
    }

    lv_obj_t *layer = lv_layer_top();
    s_mcast_modal_root = lv_obj_create(layer);
    lv_obj_set_size(s_mcast_modal_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_mcast_modal_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_mcast_modal_root, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_mcast_modal_root, 0, 0);
    lv_obj_add_event_cb(s_mcast_modal_root, on_mcast_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_mcast_modal_root);
    lv_obj_set_width(panel, 300);
    lv_obj_set_style_max_height(panel, 280, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05080a), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x00aa88), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_add_event_cb(panel, on_mcast_panel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);
    dashcdg_ui_no_scroll(panel);

    lv_obj_t *mtitle = lv_label_create(panel);
    lv_label_set_text(mtitle, "[ MCAST | RX ]");
    lv_obj_set_style_text_color(mtitle, lv_color_hex(0x66ffcc), 0);

    lv_obj_t *scroll = lv_obj_create(panel);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_min_height(scroll, 96, 0);
    lv_obj_set_style_max_height(scroll, 190, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(scroll, 2, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(scroll, on_mcast_panel_clicked, LV_EVENT_CLICKED, NULL);

    s_mcast_modal_lbl = lv_label_create(scroll);
    lv_label_set_long_mode(s_mcast_modal_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_mcast_modal_lbl, lv_pct(100));
    lv_obj_set_style_text_color(s_mcast_modal_lbl, lv_color_hex(0xaaccbb), 0);
    {
        char body[768];
        dashcdg_badge_rx_format_mcast_modal(body, sizeof(body));
        lv_label_set_text(s_mcast_modal_lbl, body);
    }

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_flex_grow(row, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_width(b, 88);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x113322), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "Close");
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_mcast_ok, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(panel);
}

static void on_tick(lv_timer_t *t)
{
    tick_ctx_t *c = (tick_ctx_t *)lv_timer_get_user_data(t);
    if (c && c->disp) {
        /*
         * CDG uses esp_lcd_panel_draw_bitmap on the same SPI bus as esp_lvgl_port. Do not repaint
         * from LV_EVENT_FLUSH_FINISH: that fires as soon as flush_cb returns while the SPI DMA for
         * LVGL may still be in flight, so overlapping CDG blits corrupt color / leave long black gaps.
         */
        if (++s_heap_retry_ticks >= 91U) {
            s_heap_retry_ticks = 0U;
            dashcdg_badge_rx_try_upgrade_cdg_heap();
        }
        dashcdg_badge_rx_cdg_overlay_tick(s_cdg_slot);
        if (s_mcast_modal_lbl && lv_obj_is_valid(s_mcast_modal_lbl)) {
            char body[768];
            dashcdg_badge_rx_format_mcast_modal(body, sizeof(body));
            lv_label_set_text(s_mcast_modal_lbl, body);
        }
    }
}

static void on_back(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_home(disp);
    }
}

esp_err_t dashcdg_karaoke_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_karaoke_ui_teardown();

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020204), 0);
    dashcdg_ui_no_scroll(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_refr_now(disp);
    lvgl_port_unlock();
    /*
     * Start RX (and try CDG/jitter calloc) while the screen is still empty so internal heap is not
     * fragmented by the karaoke LVGL tree yet.
     */
    s_heap_retry_ticks = 0U;
    dashcdg_badge_rx_start();
    if (!lvgl_port_lock(1000)) {
        dashcdg_badge_rx_stop();
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 6, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(outer, 4, 0);
    dashcdg_ui_no_scroll(outer);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 36);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(top);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_width(b_back, 72);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, "Home");
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    lv_obj_t *mid = lv_obj_create(top);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, 36);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    dashcdg_ui_no_scroll(mid);

    lv_obj_t *title = lv_label_create(mid);
    lv_label_set_text(title, "v4 CDG");
    lv_obj_set_style_text_color(title, lv_color_hex(0x66ffcc), 0);
    lv_obj_center(title);

    lv_obj_t *b_info = lv_button_create(top);
    lv_obj_set_width(b_info, 72);
    lv_obj_t *li = lv_label_create(b_info);
    lv_label_set_text(li, "(i)");
    lv_obj_center(li);
    lv_obj_add_event_cb(b_info, on_info_btn, LV_EVENT_CLICKED, disp);

    s_cdg_slot = lv_obj_create(outer);
    lv_obj_set_size(s_cdg_slot, DASHCDG_BADGE_RX_VISIBLE_W, DASHCDG_BADGE_RX_VISIBLE_H);
    lv_obj_set_style_bg_opa(s_cdg_slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cdg_slot, 1, 0);
    lv_obj_set_style_border_color(s_cdg_slot, lv_color_hex(0x335544), 0);
    lv_obj_remove_flag(s_cdg_slot, LV_OBJ_FLAG_CLICKABLE);
    dashcdg_ui_no_scroll(s_cdg_slot);

    if (!dashcdg_display_lcd_panel()) {
        ESP_LOGE(TAG, "LCD panel handle missing");
        lv_obj_t *err = lv_label_create(outer);
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(err, lv_pct(100));
        lv_obj_set_style_text_color(err, lv_color_hex(0xff8866), 0);
        lv_label_set_text(err, "Display panel not ready.");
        lv_obj_t *b = lv_button_create(outer);
        lv_obj_set_width(b, 120);
        lv_obj_t *elb = lv_label_create(b);
        lv_label_set_text(elb, "Home");
        lv_obj_center(elb);
        lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, disp);
        lv_obj_update_layout(outer);
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_update_layout(outer);
    lv_refr_now(disp);
    lvgl_port_unlock();

    s_tick_ctx.disp = disp;
    s_tick = lv_timer_create(on_tick, 33, &s_tick_ctx);
    if (!s_tick) {
        ESP_LOGW(TAG, "lv_timer_create failed");
    }

    ESP_LOGI(TAG, "karaoke / v4 proof UI up");
    return ESP_OK;
}
