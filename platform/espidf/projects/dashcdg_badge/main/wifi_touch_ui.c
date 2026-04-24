/*
 * Wi-Fi provisioning UI on LVGL + touch. Credentials stored in NVS namespace "dashcfg".
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_touch_ui.h"

static const char *TAG = "wifi_ui";
static const char *NVS_NS = "dashcfg";

static lv_obj_t *s_lbl_status;
static lv_obj_t *s_dd_ssid;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_kb;

static void ui_statusf(const char *fmt, ...)
{
    if (!s_lbl_status) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (lvgl_port_lock(1000)) {
        lv_label_set_text(s_lbl_status, buf);
        lvgl_port_unlock();
    }
}

static esp_err_t nvs_load_creds(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t l = ssid_sz;
    err = nvs_get_str(h, "ssid", ssid, &l);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    l = psk_sz;
    err = nvs_get_str(h, "psk", psk, &l);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_creds(const char *ssid, const char *psk)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "ssid", ssid), TAG, "set ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "psk", psk), TAG, "set psk");
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t nvs_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "psk");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void rebuild_dropdown_from_scan(void)
{
    wifi_ap_record_t recs[40];
    memset(recs, 0, sizeof(recs));
    uint16_t n = 40;
    esp_err_t err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK || n == 0) {
        ui_statusf("Scan: no APs (%s)", esp_err_to_name(err));
        return;
    }

    char opts[2048];
    size_t off = 0;
    off += snprintf(opts + off, sizeof(opts) - off, "%s", "(select SSID)");

    for (unsigned i = 0; i < n && off < sizeof(opts) - 64; i++) {
        char line[40];
        const uint8_t *s = recs[i].ssid;
        size_t sl = strnlen((const char *)s, sizeof(recs[i].ssid));
        if (sl == 0) {
            continue;
        }
        memcpy(line, s, sl);
        line[sl] = 0;
        off += snprintf(opts + off, sizeof(opts) - off, "\n%s", line);
    }

    if (lvgl_port_lock(1000)) {
        lv_dropdown_set_options(s_dd_ssid, opts);
        lvgl_port_unlock();
    }
    ui_statusf("Scan: found networks — pick SSID");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        rebuild_dropdown_from_scan();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ui_statusf("Wi-Fi started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ui_statusf("Disconnected (tap Connect after Scan)");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ui_statusf("Online\nIP: " IPSTR, IP2STR(&ev->ip_info.ip));

        wifi_config_t wc = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            nvs_save_creds((const char *)wc.sta.ssid, (const char *)wc.sta.password);
        }
    }
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    ui_statusf("Scanning…");
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ui_statusf("Scan start failed: %s", esp_err_to_name(err));
    }
}

static void on_connect(lv_event_t *e)
{
    (void)e;
    char ssid[33] = {0};
    char psk[65] = {0};

    if (lvgl_port_lock(1000)) {
        lv_dropdown_get_selected_str(s_dd_ssid, ssid, sizeof(ssid));
        const char *pw = lv_textarea_get_text(s_ta_pass);
        strncpy(psk, pw, sizeof(psk) - 1);
        lvgl_port_unlock();
    }

    if (ssid[0] == 0 || strcmp(ssid, "(select SSID)") == 0) {
        ui_statusf("Pick an SSID from the list");
        return;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, psk, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ui_statusf("set_config: %s", esp_err_to_name(err));
        return;
    }
    ui_statusf("Connecting to\n%s …", ssid);
    err = esp_wifi_disconnect();
    (void)err;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ui_statusf("connect: %s", esp_err_to_name(err));
    }
}

static void on_forget(lv_event_t *e)
{
    (void)e;
    (void)nvs_clear_creds();
    esp_wifi_disconnect();
    ui_statusf("Forgot saved Wi-Fi");
    if (lvgl_port_lock(1000)) {
        lv_textarea_set_text(s_ta_pass, "");
        lvgl_port_unlock();
    }
}

static void build_ui(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0a12), 0);

    /* Scroll root: default LVGL keyboard is taller than 320px column with other widgets. */
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_STRETCH, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "dashcdg badge · Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(0xb0ffe8), 0);

    s_lbl_status = lv_label_create(root);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, lv_pct(100));
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xe0e0e8), 0);
    lv_label_set_text(s_lbl_status, "Ready");

    s_dd_ssid = lv_dropdown_create(root);
    lv_obj_set_width(s_dd_ssid, lv_pct(100));
    lv_dropdown_set_options(s_dd_ssid, "(select SSID)\nTap SCAN");

    s_ta_pass = lv_textarea_create(root);
    lv_obj_set_width(s_ta_pass, lv_pct(100));
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "WPA passphrase");

    /* Buttons before keyboard so Scan/Connect stay reachable on a 320px-tall panel. */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_scan = lv_button_create(row);
    lv_obj_set_flex_grow(b_scan, 1);
    lv_obj_t *l1 = lv_label_create(b_scan);
    lv_label_set_text(l1, "Scan");
    lv_obj_center(l1);
    lv_obj_add_event_cb(b_scan, on_scan, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_go = lv_button_create(row);
    lv_obj_set_flex_grow(b_go, 1);
    lv_obj_t *l2 = lv_label_create(b_go);
    lv_label_set_text(l2, "Connect");
    lv_obj_center(l2);
    lv_obj_add_event_cb(b_go, on_connect, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_forget = lv_button_create(row);
    lv_obj_set_flex_grow(b_forget, 1);
    lv_obj_t *l3 = lv_label_create(b_forget);
    lv_label_set_text(l3, "Forget");
    lv_obj_center(l3);
    lv_obj_add_event_cb(b_forget, on_forget, LV_EVENT_CLICKED, NULL);

    s_kb = lv_keyboard_create(root);
    lv_obj_set_width(s_kb, lv_pct(100));
    lv_obj_set_style_max_height(s_kb, 150, 0);
    lv_keyboard_set_textarea(s_kb, s_ta_pass);
}

static esp_err_t try_auto_connect_saved(void)
{
    char ssid[65] = {0};
    char psk[65] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, psk, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set saved");
    ui_statusf("Auto-connect…\n%s", ssid);
    return esp_wifi_connect();
}

esp_err_t dashcdg_wifi_touch_ui_start(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL), TAG,
                        "reg wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL), TAG,
                        "reg ip");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    if (lvgl_port_lock(1000)) {
        build_ui(disp);
        lvgl_port_unlock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    try_auto_connect_saved();

    return ESP_OK;
}
