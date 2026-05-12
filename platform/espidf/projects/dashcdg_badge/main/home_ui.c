/*
 * Launcher + status dock. "Terminal phreak" vibe; compact tiles; sysinfo modal.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "battery_label.h"
#include "board_cyd_freenove_32.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "vbat_sense.h"
#include "wifi_touch_ui.h"

/* TODO(remove): on-screen XPT2046 read snapshot (LVGL-mapped x/y + z + PENIRQ). Set 0 before ship. */
#define DASHCDG_HOME_TOUCH_DEBUG_OVERLAY 0

static const char *TAG = "home_ui";

static lv_timer_t *s_status_timer;
#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
static lv_timer_t *s_touch_dbg_timer;
static lv_obj_t *s_touch_dbg_lbl;
#endif
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_lbl_bat;
static lv_obj_t *s_info_modal;
/** Sysinfo modal: battery block label + timer (LVGL thread; no port lock in cb). */
static lv_obj_t *s_info_pack_lbl;
static lv_timer_t *s_info_live_tmr;

/** Full-width low-pack warning on `lv_layer_top` (dismiss tap; re-arms above clear mV). */
static lv_obj_t *s_lowbat_root;
static lv_obj_t *s_lowbat_title;
static lv_obj_t *s_lowbat_sub;
static lv_timer_t *s_lowbat_anim_timer;
static uint8_t s_lowbat_user_dismissed;
static uint8_t s_lowbat_anim_step;

/** Show bar at/under this pack mV; hide until re-armed after reaching CLEAR mV. */
#define HOME_LOWBAT_SHOW_MV  3400
#define HOME_LOWBAT_CLEAR_MV 3520

/** Battery label: use default style selector `0` (same as karaoke bar) so tint updates apply reliably. */
static void home_bat_label_init_style(lv_obj_t *lbl_bat)
{
    if (!lbl_bat) {
        return;
    }
    lv_obj_set_style_text_color(lbl_bat, lv_color_hex(0xaaccbb), 0);
}

/** LVGL 9 defaults lv_obj to scrollable; tiny flex overflows draw scrollbar chrome on the status dock. */
static void dashcdg_ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

/**
 * Height for each home launcher tile, derived from `CYD_LCD_V_RES` and the real layout in
 * `dashcdg_home_ui_present` (outer pad, header row, three outer pad_row gaps, tiles wrapper pad,
 * gap between tiles, per-tile col pad, status bar) plus a small reserve so the dock stays visible.
 */
static lv_coord_t home_launcher_tile_height_px(void)
{
    const int v = CYD_LCD_V_RES;
    /* Flex column height = 6 + 32 + 4 + (2+(T+4)+4+(T+4)+2) + 4 + spacer + 4 + 30 + 6 = 2T + 102
     * (spacer min 0). Matches `dashcdg_home_ui_present` padding and row gaps. */
    const int non_tile = 102;
    const int extra_reserve = 26;
    int t = (v - non_tile - extra_reserve) / 2;
    if (t < 44) {
        t = 44;
    }
    if (t > 56) {
        t = 56;
    }
    return (lv_coord_t)t;
}

static void on_tile_wifi(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_wifi(disp);
    }
}

static void on_tile_karaoke(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_karaoke(disp);
    }
}

static void on_tile_settings(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_settings(disp);
    }
}

static void on_tile_applications(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_applications(disp);
    }
}

static void sysinfo_modal_close(void)
{
    if (s_info_live_tmr) {
        lv_timer_del(s_info_live_tmr);
        s_info_live_tmr = NULL;
    }
    s_info_pack_lbl = NULL;
    if (s_info_modal && lv_obj_is_valid(s_info_modal)) {
        lv_obj_del(s_info_modal);
    }
    s_info_modal = NULL;
}

static void on_modal_scrim_clicked(lv_event_t *e)
{
    (void)e;
    sysinfo_modal_close();
}

static void on_modal_panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void on_modal_ok_click(lv_event_t *e)
{
    (void)e;
    sysinfo_modal_close();
}

static esp_err_t home_battery_read_once(int *out_raw, int *out_pin_mv, int *out_vbat_mv)
{
    int raw = 0;
    int pin = 0;
    int vbat = 0;
    esp_err_t br = ESP_FAIL;

    if (dashcdg_platform_hw_is_ready()) {
        br = dashcdg_platform_hw_battery_read(&raw, &pin, &vbat);
    }
    if (br != ESP_OK) {
        pin = 0;
        if (!dashcdg_vbat_sense_is_ready() || dashcdg_vbat_sense_read(&raw, &pin, &vbat) != ESP_OK) {
            return ESP_FAIL;
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

static void home_sysinfo_refresh_pack_lbl(void)
{
    if (!s_info_pack_lbl || !lv_obj_is_valid(s_info_pack_lbl)) {
        return;
    }
    int braw = 0;
    int pin_mv = 0;
    int bmv = 0;
    char buf[320];
    if (home_battery_read_once(&braw, &pin_mv, &bmv) != ESP_OK) {
        snprintf(buf, sizeof(buf), "--- Pack (ADC) ---\n(read failed)\n");
        lv_label_set_text(s_info_pack_lbl, buf);
        return;
    }
    snprintf(buf, sizeof(buf),
             "--- Pack (ADC) ---\n"
             "raw .... %d   (log at shutdown for cal)\n"
             "pin .... %d mV\n"
             "pack ... %d mV\n"
             "dock tint: red low %d .. blue mid %d .. green high %d mV\n",
             braw, pin_mv, bmv, DASHCDG_BAT_COLOR_MV_LOW, DASHCDG_BAT_COLOR_MV_MID, DASHCDG_BAT_COLOR_MV_FULL);
    lv_label_set_text(s_info_pack_lbl, buf);
}

static void home_sysinfo_live_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_info_modal || !lv_obj_is_valid(s_info_modal) || !s_info_pack_lbl || !lv_obj_is_valid(s_info_pack_lbl)) {
        if (s_info_live_tmr) {
            lv_timer_del(s_info_live_tmr);
            s_info_live_tmr = NULL;
        }
        s_info_pack_lbl = NULL;
        return;
    }
    home_sysinfo_refresh_pack_lbl();
}

static void home_lowbat_overlay_destroy(void)
{
    if (s_lowbat_anim_timer) {
        lv_timer_del(s_lowbat_anim_timer);
        s_lowbat_anim_timer = NULL;
    }
    if (s_lowbat_root && lv_obj_is_valid(s_lowbat_root)) {
        lv_obj_del(s_lowbat_root);
    }
    s_lowbat_root = NULL;
    s_lowbat_title = NULL;
    s_lowbat_sub = NULL;
}

static void home_lowbat_anim_timer_cb(lv_timer_t *t)
{
    if (!s_lowbat_root || !lv_obj_is_valid(s_lowbat_root)) {
        lv_timer_del(t);
        s_lowbat_anim_timer = NULL;
        return;
    }
    s_lowbat_anim_step++;
    lv_obj_set_style_bg_color(s_lowbat_root, (s_lowbat_anim_step & 1U) ? lv_color_hex(0x3a1510) : lv_color_hex(0x4a2010), 0);
    lv_obj_set_style_border_color(s_lowbat_root, (s_lowbat_anim_step & 2U) ? lv_color_hex(0xff9933) : lv_color_hex(0xff5522), 0);
}

static void home_lowbat_overlay_touch_dismiss(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_lowbat_user_dismissed = 1U;
    home_lowbat_overlay_destroy();
}

static void home_lowbat_overlay_update(int raw, int vbat_mv)
{
    if (vbat_mv >= HOME_LOWBAT_CLEAR_MV) {
        s_lowbat_user_dismissed = 0U;
        home_lowbat_overlay_destroy();
        return;
    }
    if (s_lowbat_user_dismissed) {
        return;
    }
    if (vbat_mv > HOME_LOWBAT_SHOW_MV) {
        return;
    }

    int deci = (vbat_mv * 10 + 500) / 1000;
    if (deci < 0) {
        deci = 0;
    }
    int w = deci / 10;
    int f = deci % 10;

    if (!s_lowbat_root || !lv_obj_is_valid(s_lowbat_root)) {
        s_lowbat_root = lv_obj_create(lv_layer_top());
        lv_obj_set_width(s_lowbat_root, lv_pct(100));
        lv_obj_set_height(s_lowbat_root, LV_SIZE_CONTENT);
        lv_obj_align(s_lowbat_root, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_opa(s_lowbat_root, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_lowbat_root, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_lowbat_root, lv_color_hex(0x3a1510), 0);
        lv_obj_set_style_bg_grad_dir(s_lowbat_root, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_shadow_width(s_lowbat_root, 0, 0);
        lv_obj_set_style_outline_width(s_lowbat_root, 0, 0);
        lv_obj_set_style_border_side(s_lowbat_root, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_lowbat_root, 4, 0);
        lv_obj_set_style_border_color(s_lowbat_root, lv_color_hex(0xff7722), 0);
        lv_obj_set_style_pad_hor(s_lowbat_root, 10, 0);
        lv_obj_set_style_pad_ver(s_lowbat_root, 8, 0);
        lv_obj_set_style_pad_row(s_lowbat_root, 4, 0);
        lv_obj_set_style_radius(s_lowbat_root, 0, 0);
        lv_obj_set_flex_flow(s_lowbat_root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_lowbat_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(s_lowbat_root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_lowbat_root, home_lowbat_overlay_touch_dismiss, LV_EVENT_CLICKED, NULL);
        lv_obj_remove_flag(s_lowbat_root, LV_OBJ_FLAG_SCROLLABLE);

        s_lowbat_title = lv_label_create(s_lowbat_root);
        lv_label_set_long_mode(s_lowbat_title, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s_lowbat_title, lv_pct(92));
        lv_obj_set_style_text_color(s_lowbat_title, lv_color_hex(0xfff2e6), 0);
        lv_obj_set_style_text_align(s_lowbat_title, LV_TEXT_ALIGN_CENTER, 0);

        s_lowbat_sub = lv_label_create(s_lowbat_root);
        lv_label_set_long_mode(s_lowbat_sub, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s_lowbat_sub, lv_pct(92));
        lv_obj_set_style_text_color(s_lowbat_sub, lv_color_hex(0xd4b8a8), 0);
        lv_obj_set_style_text_align(s_lowbat_sub, LV_TEXT_ALIGN_CENTER, 0);
        {
            int sw = HOME_LOWBAT_SHOW_MV / 1000;
            int sf = (HOME_LOWBAT_SHOW_MV % 1000) / 10;
            int cw = HOME_LOWBAT_CLEAR_MV / 1000;
            int cf = (HOME_LOWBAT_CLEAR_MV % 1000) / 10;
            char sub[160];
            snprintf(sub, sizeof(sub),
                     "Tap to hide. Comes back if the pack stays at or under %d.%02d V until it reads above "
                     "%d.%02d V while charging.",
                     sw, sf, cw, cf);
            lv_label_set_text(s_lowbat_sub, sub);
        }

        s_lowbat_anim_step = 0U;
        if (s_lowbat_anim_timer == NULL) {
            s_lowbat_anim_timer = lv_timer_create(home_lowbat_anim_timer_cb, 320, NULL);
        }
        lv_obj_move_foreground(s_lowbat_root);
    }

    if (s_lowbat_title && lv_obj_is_valid(s_lowbat_title)) {
        char line[96];
        snprintf(line, sizeof(line), LV_SYMBOL_BATTERY_EMPTY "  Low battery  %d.%dV", w, f);
        lv_label_set_text(s_lowbat_title, line);
    }
}

static void on_info_btn(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (!disp || s_info_modal) {
        return;
    }

    sysinfo_modal_close();

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);

    char body[960];
    const esp_app_desc_t *app = esp_app_get_description();

    snprintf(body, sizeof(body),
             "> dashcdg_badge payload\n"
             "fw_ver .. %s\n"
             "idf ..... %s\n"
             "proj .... %s\n",
             app->version[0] ? app->version : "?",
             app->idf_ver,
             app->project_name[0] ? app->project_name : "dashcdg");

    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        size_t z0 = strlen(body);
        snprintf(body + z0, sizeof(body) - z0, "\n[wifi] stack not up\n");
    } else if (werr == ESP_OK) {
        char ip_s[20] = "--";
        char gw_s[20] = "--";
        char nm_s[20] = "--";
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK) {
                snprintf(ip_s, sizeof(ip_s), IPSTR, IP2STR(&ipi.ip));
                snprintf(gw_s, sizeof(gw_s), IPSTR, IP2STR(&ipi.gw));
                snprintf(nm_s, sizeof(nm_s), IPSTR, IP2STR(&ipi.netmask));
            }
        }
        char bssid_s[24] = "??:??:??";
        if (ap.bssid[0] || ap.bssid[1]) {
            snprintf(bssid_s, sizeof(bssid_s), "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1],
                     ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        }
        size_t z = strlen(body);
        snprintf(body + z, sizeof(body) - z,
                 "\n--- STA dump ---\n"
                 "ssid .... %s\n"
                 "bssid ... %s\n"
                 "ch ...... %u\n"
                 "rssi .... %d dBm\n"
                 "ip ...... %s\n"
                 "gw ...... %s\n"
                 "mask .... %s\n",
                 (const char *)ap.ssid, bssid_s, (unsigned)ap.primary, ap.rssi, ip_s, gw_s, nm_s);
    } else {
        size_t z1 = strlen(body);
        snprintf(body + z1, sizeof(body) - z1, "\n[wifi] idle / disconnected\n");
    }

    lv_obj_t *layer = lv_layer_top();
    s_info_modal = lv_obj_create(layer);
    lv_obj_set_size(s_info_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_info_modal, lv_color_hex(0x080a09), 0);
    lv_obj_set_style_bg_opa(s_info_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_info_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_info_modal, 0, 0);
    lv_obj_add_event_cb(s_info_modal, on_modal_scrim_clicked, LV_EVENT_CLICKED, NULL);
    dashcdg_ui_no_scroll(s_info_modal);
    lv_obj_remove_flag(s_info_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_info_modal);
    lv_obj_set_width(panel, lv_pct(90));
    lv_obj_set_height(panel, lv_pct(72));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05080a), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x00aa88), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_add_event_cb(panel, on_modal_panel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);
    dashcdg_ui_no_scroll(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "[ SYS | INF0 ]");
    lv_obj_set_style_text_color(title, lv_color_hex(0x66ffcc), 0);

    lv_obj_t *scroll = lv_obj_create(panel);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_min_height(scroll, 120, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(scroll, 4, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(scroll, on_modal_panel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(scroll, 6, 0);

    lv_obj_t *txt = lv_label_create(scroll);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(txt, lv_pct(100));
    lv_obj_set_style_text_color(txt, lv_color_hex(0xaaccbb), 0);
    lv_label_set_text(txt, body);

    lv_obj_t *pack_lbl = lv_label_create(scroll);
    s_info_pack_lbl = pack_lbl;
    lv_label_set_long_mode(pack_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(pack_lbl, lv_pct(100));
    lv_obj_set_style_text_color(pack_lbl, lv_color_hex(0xaaccbb), 0);
    home_sysinfo_refresh_pack_lbl();
    s_info_live_tmr = lv_timer_create(home_sysinfo_live_timer_cb, 500, NULL);

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
    lv_obj_set_width(b, 72);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "> ok");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x66ffcc), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_modal_ok_click, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(panel);
    lv_obj_move_foreground(s_info_modal);
}

void dashcdg_home_ui_pause_status_updates(void)
{
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
    if (s_touch_dbg_timer) {
        lv_timer_del(s_touch_dbg_timer);
        s_touch_dbg_timer = NULL;
    }
    s_touch_dbg_lbl = NULL;
#endif
    s_lbl_wifi = NULL;
    s_lbl_ip = NULL;
    s_lbl_bat = NULL;
    sysinfo_modal_close();
    home_lowbat_overlay_destroy();
}

static void home_status_fill_bat(char *bat_txt, size_t bat_sz, lv_color_t *bat_col)
{
    int raw = 0;
    int vbat = 0;
    if (home_battery_read_once(&raw, NULL, &vbat) != ESP_OK) {
        snprintf(bat_txt, bat_sz, LV_SYMBOL_BATTERY_EMPTY " --");
        *bat_col = lv_color_hex(0x887766);
        return;
    }
    dashcdg_battery_format_status_line_raw(bat_txt, bat_sz, vbat, raw);
    *bat_col = dashcdg_battery_label_color_from_pack_mv(vbat);
}

#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
static void home_touch_dbg_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_touch_dbg_lbl || !lv_obj_is_valid(s_touch_dbg_lbl)) {
        return;
    }
    char line[120];
    dashcdg_touch_debug_format_line(line, sizeof(line));
    lv_label_set_text(s_touch_dbg_lbl, line);
}
#endif

/*
 * LVGL timers run on the LVGL task - never lvgl_port_lock from timer cb.
 */
static void home_status_gather(char *wifi_txt, size_t wifi_sz, char *ip_txt, size_t ip_sz, char *bat_txt, size_t bat_sz,
                               lv_color_t *bat_col)
{
    home_status_fill_bat(bat_txt, bat_sz, bat_col);

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));

    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);
    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        snprintf(wifi_txt, wifi_sz, "[wifi] off");
        snprintf(ip_txt, ip_sz, "--.--.--.--");
    } else if (werr == ESP_OK) {
        const char *ssid = (const char *)ap.ssid;
        /* One line: truncate SSID hard so the bar never wraps on long names */
        if (ssid[0]) {
            if (strlen(ssid) > 10) {
                snprintf(wifi_txt, wifi_sz, "%.8s~ %ddBm", ssid, ap.rssi);
            } else {
                snprintf(wifi_txt, wifi_sz, "%s %ddBm", ssid, ap.rssi);
            }
        } else {
            snprintf(wifi_txt, wifi_sz, "assoc %ddBm", ap.rssi);
        }
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                snprintf(ip_txt, ip_sz, IPSTR, IP2STR(&ipi.ip));
            } else {
                snprintf(ip_txt, ip_sz, "pending");
            }
        } else {
            snprintf(ip_txt, ip_sz, "no if");
        }
    } else {
        snprintf(wifi_txt, wifi_sz, "[wifi] idle");
        snprintf(ip_txt, ip_sz, "--.--.--.--");
    }
}

static void home_status_apply_labels(const char *wifi_txt, const char *ip_txt, const char *bat_txt, lv_color_t bat_col)
{
    lv_label_set_text(s_lbl_wifi, wifi_txt);
    lv_label_set_text(s_lbl_ip, ip_txt);
    if (s_lbl_bat) {
        lv_obj_set_style_text_color(s_lbl_bat, bat_col, 0);
        lv_label_set_text(s_lbl_bat, bat_txt);
        lv_obj_invalidate(s_lbl_bat);
    }
}

static void home_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_wifi || !s_lbl_ip || !s_lbl_bat) {
        return;
    }
    if (!lv_obj_is_valid(s_lbl_wifi) || !lv_obj_is_valid(s_lbl_ip) || !lv_obj_is_valid(s_lbl_bat)) {
        dashcdg_home_ui_pause_status_updates();
        return;
    }

    char wifi_txt[48];
    char ip_txt[20];
    char bat_txt[28];
    lv_color_t bat_col;
    home_status_gather(wifi_txt, sizeof(wifi_txt), ip_txt, sizeof(ip_txt), bat_txt, sizeof(bat_txt), &bat_col);
    {
        int br = 0;
        int bmv = 0;
        if (home_battery_read_once(&br, NULL, &bmv) == ESP_OK) {
            home_lowbat_overlay_update(br, bmv);
        } else {
            home_lowbat_overlay_destroy();
        }
    }
    home_status_apply_labels(wifi_txt, ip_txt, bat_txt, bat_col);
}

static void home_status_refresh_foreign(void)
{
    if (!s_lbl_wifi || !s_lbl_ip || !s_lbl_bat) {
        return;
    }

    char wifi_txt[48];
    char ip_txt[20];
    char bat_txt[28];
    lv_color_t bat_col;
    home_status_gather(wifi_txt, sizeof(wifi_txt), ip_txt, sizeof(ip_txt), bat_txt, sizeof(bat_txt), &bat_col);

    if (!lvgl_port_lock(400)) {
        return;
    }
    {
        int br = 0;
        int bmv = 0;
        if (home_battery_read_once(&br, NULL, &bmv) == ESP_OK) {
            home_lowbat_overlay_update(br, bmv);
        }
    }
    home_status_apply_labels(wifi_txt, ip_txt, bat_txt, bat_col);
    lvgl_port_unlock();
}

/**
 * Compact tile: in-button label. `cb` NULL = disabled placeholder (no tap); settings stay on header gear.
 * `tile_h` from `home_launcher_tile_height_px()`.
 */
static lv_obj_t *make_app_tile(lv_obj_t *parent, const char *btn_lines, lv_coord_t tile_h, lv_event_cb_t cb,
                               lv_disp_t *disp)
{
    lv_obj_t *col = lv_obj_create(parent);
    /* Never flex_grow the wrapper in a column: two grow siblings split free height and leave a
     * huge empty band between stacked tiles. */
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 2, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    dashcdg_ui_no_scroll(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *tile = lv_button_create(col);
    lv_obj_set_width(tile, lv_pct(100));
    lv_obj_set_height(tile, tile_h);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0a1410), 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_shadow_width(tile, 2, 0);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x002211), 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, disp);
    } else {
        lv_obj_add_state(tile, LV_STATE_DISABLED);
    }

    lv_obj_t *lg = lv_label_create(tile);
    lv_label_set_long_mode(lg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lg, lv_pct(94));
    lv_label_set_text(lg, btn_lines);
    lv_obj_set_style_text_align(lg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lg, lv_color_hex(0x66ffaa), 0);
    lv_obj_center(lg);

    return col;
}

esp_err_t dashcdg_home_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    dashcdg_home_ui_pause_status_updates();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020403), 0);
    dashcdg_ui_no_scroll(scr);
    /* Default screen is clickable and full-screen; if hit-testing misses children, LVGL targets scr and taps die. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 6, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(outer);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr_row = lv_obj_create(outer);
    lv_obj_set_width(hdr_row, lv_pct(100));
    lv_obj_set_height(hdr_row, 32);
    /* Match status bar horizontal inset so trailing controls share one vertical line (gear right = info "?"). */
    lv_obj_set_style_pad_left(hdr_row, 6, 0);
    lv_obj_set_style_pad_right(hdr_row, 4, 0);
    lv_obj_set_style_pad_top(hdr_row, 0, 0);
    lv_obj_set_style_pad_bottom(hdr_row, 0, 0);
    lv_obj_set_style_border_width(hdr_row, 0, 0);
    lv_obj_set_style_bg_opa(hdr_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(hdr_row);
    lv_obj_remove_flag(hdr_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr = lv_label_create(hdr_row);
    lv_label_set_text(hdr, "> Shadyvision // BADGE");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x33ff99), 0);
    lv_obj_set_flex_grow(hdr, 1);
    lv_obj_set_style_min_width(hdr, 0, 0);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_MODE_CLIP);

    lv_obj_t *b_gear = lv_button_create(hdr_row);
    lv_obj_set_size(b_gear, 26, 22);
    lv_obj_set_style_pad_all(b_gear, 0, 0);
    lv_obj_set_style_shadow_width(b_gear, 0, 0);
    lv_obj_set_style_bg_color(b_gear, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b_gear, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b_gear, 1, 0);
    lv_obj_set_style_radius(b_gear, 3, 0);
    lv_obj_t *gl = lv_label_create(b_gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gl, lv_color_hex(0x88ffcc), 0);
    lv_obj_center(gl);
    lv_obj_add_event_cb(b_gear, on_tile_settings, LV_EVENT_CLICKED, disp);

    /* Two full-width tiles only: karaoke, then settings (no flex-grow spacer - it ate layout and
     * pushed the status dock off-screen / clipped tiles). */
    lv_obj_t *tiles = lv_obj_create(outer);
    lv_obj_set_width(tiles, lv_pct(100));
    lv_obj_set_height(tiles, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tiles, 2, 0);
    lv_obj_set_style_border_width(tiles, 0, 0);
    lv_obj_set_style_bg_opa(tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tiles, 3, 0);
    dashcdg_ui_no_scroll(tiles);
    lv_obj_remove_flag(tiles, LV_OBJ_FLAG_CLICKABLE);
    {
        const lv_coord_t th = home_launcher_tile_height_px();
        /* Line 1 ASCII; line 2: three Montserrat symbols (mic-ish + video + loop). */
        make_app_tile(tiles,
                       "mcast / karaoke\n" LV_SYMBOL_AUDIO "  " LV_SYMBOL_VIDEO "  " LV_SYMBOL_LOOP,
                       th,
                       on_tile_karaoke,
                       disp);
        /* Applications hub (Audio lab, …). */
        make_app_tile(tiles,
                       "Applications\n" LV_SYMBOL_AUDIO "  " LV_SYMBOL_LIST "  " LV_SYMBOL_KEYBOARD,
                       th,
                       on_tile_applications,
                       disp);
    }

    /* Push status bar to the physical bottom; only this spacer flex-grows (not inside tile column). */
    lv_obj_t *dock_spacer = lv_obj_create(outer);
    lv_obj_set_width(dock_spacer, lv_pct(100));
    lv_obj_set_flex_grow(dock_spacer, 1);
    lv_obj_set_style_min_height(dock_spacer, 0, 0);
    lv_obj_set_style_bg_opa(dock_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dock_spacer, 0, 0);
    dashcdg_ui_no_scroll(dock_spacer);
    lv_obj_remove_flag(dock_spacer, LV_OBJ_FLAG_CLICKABLE);

    /* Status dock: single-line fields + info btn */
    lv_obj_t *bar = lv_obj_create(outer);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_pad_left(bar, 6, 0);
    lv_obj_set_style_pad_right(bar, 4, 0);
    lv_obj_set_style_pad_top(bar, 4, 0);
    lv_obj_set_style_pad_bottom(bar, 4, 0);
    /* Inner width is hres minus outer pad minus bar pad; fixed children + gaps must fit (was
     * clipping the trailing "?" control on 320px landscape). */
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x030605), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x115533), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(bar, 0);
    dashcdg_ui_no_scroll(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_wifi = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_wifi, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_wifi, 90);
    lv_obj_set_height(s_lbl_wifi, 18);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(0x66ddaa), 0);
    lv_label_set_text(s_lbl_wifi, "[wifi] ...");
    lv_obj_add_flag(s_lbl_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_wifi, on_tile_wifi, LV_EVENT_CLICKED, disp);

    s_lbl_ip = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_ip, 72);
    lv_obj_set_height(s_lbl_ip, 18);
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0xaaccff), 0);
    lv_obj_set_style_text_align(s_lbl_ip, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_lbl_ip, "--");

    s_lbl_bat = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_bat, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_bat, 94);
    lv_obj_set_height(s_lbl_bat, 18);
    home_bat_label_init_style(s_lbl_bat);
    lv_obj_set_style_text_align(s_lbl_bat, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_lbl_bat, " -- ");

    lv_obj_t *sp = lv_obj_create(bar);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    dashcdg_ui_no_scroll(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *info = lv_button_create(bar);
    lv_obj_set_size(info, 26, 22);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_shadow_width(info, 0, 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_radius(info, 3, 0);
    lv_obj_t *il = lv_label_create(info);
    lv_label_set_text(il, "?");
    lv_obj_set_style_text_color(il, lv_color_hex(0x88ffcc), 0);
    lv_obj_center(il);
    lv_obj_add_event_cb(info, on_info_btn, LV_EVENT_CLICKED, disp);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    /* touch_cal_ui disables the indev; after lv_obj_clean, drop stale press/hover on deleted objs. */
    dashcdg_touch_rearm_locked();

#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
    s_touch_dbg_lbl = lv_label_create(scr);
    lv_label_set_long_mode(s_touch_dbg_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_touch_dbg_lbl, 316, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_touch_dbg_lbl, 2, 2);
    lv_obj_set_style_text_color(s_touch_dbg_lbl, lv_color_hex(0xffcc00), 0);
    lv_obj_set_style_bg_color(s_touch_dbg_lbl, lv_color_hex(0x0a0500), 0);
    lv_obj_set_style_bg_opa(s_touch_dbg_lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(s_touch_dbg_lbl, 4, 0);
    lv_obj_set_style_pad_ver(s_touch_dbg_lbl, 2, 0);
    lv_obj_set_style_radius(s_touch_dbg_lbl, 2, 0);
    dashcdg_ui_no_scroll(s_touch_dbg_lbl);
    lv_obj_remove_flag(s_touch_dbg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_touch_dbg_lbl, "tp dbg...");
    lv_obj_move_foreground(s_touch_dbg_lbl);
    if (s_touch_dbg_timer) {
        lv_timer_del(s_touch_dbg_timer);
        s_touch_dbg_timer = NULL;
    }
    s_touch_dbg_timer = lv_timer_create(home_touch_dbg_timer_cb, 80, NULL);
#endif

    lvgl_port_unlock();

    home_status_refresh_foreign();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_status_timer = lv_timer_create(home_status_timer_cb, 3000, NULL);
    lvgl_port_unlock();

    dashcdg_wifi_debug_try_autolaunch_after_home(disp);
    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_HOME);
    return ESP_OK;
}
