/*
 * v4 multicast CDG proof: full CDG viewport + panel band blit; mcast diagnostics in (i) modal.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "battery_label.h"
#include "badge_prefs.h"
#include "badge_rx.h"
#include "dashcdg/media_clock.h"
#include "display_lvgl.h"
#include "karaoke_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "vbat_sense.h"

static const char *TAG = "karaoke_ui";

static lv_timer_t *s_tick;
/** Deferred lv_obj_del(modal): avoids LVGL reentrancy crashes closing from CLICKED. */
static lv_timer_t *s_mcast_modal_close_timer;
static lv_obj_t *s_mcast_modal_close_root;

/** Modal dashboard cards; refreshed while open (NULL when closed). */
static lv_obj_t *s_mcast_card_net;
static lv_obj_t *s_mcast_card_stream;
static lv_obj_t *s_mcast_card_repair;
static lv_obj_t *s_mcast_card_memory;
static lv_obj_t *s_mcast_card_system;
static lv_obj_t *s_mcast_card_song;
static lv_obj_t *s_mcast_modal_root;
/** Layout anchor + border; pixels drawn via esp_lcd_panel_draw_bitmap (see badge_rx_ui_tick). */
static lv_obj_t *s_cdg_slot;
static lv_obj_t *s_cdg_frame;
static lv_obj_t *s_stage_fill;
static lv_obj_t *s_audio_only_panel;
static lv_obj_t *s_audio_track_lbl;
static lv_obj_t *s_audio_vol_slider;
static lv_obj_t *s_audio_vol_pct_lbl;
static lv_obj_t *s_audio_mute_btn;
static lv_obj_t *s_audio_mute_lbl;
static bool s_audio_vol_slider_guard;
static uint8_t s_audio_vol_before_mute = 85U;
static bool s_was_audio_only_mode;
/** Cached NV karaoke decode prefs; refreshed only in `present` and `dashcdg_karaoke_ui_sync_decode_layout_from_prefs`. */
static uint8_t s_kui_layout_pref_video = 1U;
static uint8_t s_kui_layout_pref_audio = 1U;

static lv_obj_t *s_bar_wifi;
static lv_obj_t *s_bar_bat;
static lv_obj_t *s_bar_m;
static lv_obj_t *s_bar_c;
static lv_obj_t *s_bar_d;
static lv_obj_t *s_bar_line;
static uint64_t s_last_buf_nonzero_ms;
static uint64_t s_last_m_progress_ms;
static uint64_t s_last_c_progress_ms;
static uint64_t s_last_d_progress_ms;
static uint64_t s_last_q_sample_ms;
static uint64_t s_last_q_datagrams;
static uint64_t s_last_q_wire_miss;
static uint32_t s_last_q_cdg_miss;
static uint32_t s_last_q_wire_reorder;
static uint32_t s_last_q_parse_fail;
static uint32_t s_last_q_repair_fail;
static uint32_t s_last_q_miss_per_10k;
static uint32_t s_last_q_reorder_per_10k;
static uint32_t s_loss_hist_per_10k[5];
static uint8_t s_loss_hist_count;
static uint8_t s_loss_hist_head;
static uint32_t s_loss_hist_sum_per_10k;

typedef struct {
    lv_disp_t *disp;
} tick_ctx_t;

static tick_ctx_t s_tick_ctx;
static uint32_t s_heap_retry_ticks;
/** Modal body refresh cadence (LVGL ticks); heavy dashboard work is throttled (T6). */
static uint16_t s_mcast_modal_body_ticks;
/** Cached heap snapshot for mcast modal SYSTEM card (refresh every N dashboard updates). */
static unsigned long s_mcast_dash_heap_free;
static unsigned long s_mcast_dash_heap_min;
static unsigned long s_mcast_dash_int_largest;
static uint8_t s_mcast_dash_heap_refresh_i;
/** Next time (ms) to refresh Wi-Fi + battery in the status dock (~phone-like cadence). */
static uint64_t s_status_slow_deadline_ms;

/** Wi-Fi / bat label refresh interval (also bounds `platform_hw` ADC reads from this UI path). */
#define KARAOKE_STATUS_SLOW_PERIOD_MS 2500U
/** Loss smoothing window uses 2s quality samples x 5 entries (~10s rolling average). */
#define KARAOKE_LOSS_AVG_SAMPLES      5U
/** Gap (px) between the header row and the CDG slot; tune for spacing vs vertical budget. */
#define KARAOKE_HEADER_TO_CDG_GAP_PX 10
/** Decorative frame around the panel blit viewport (CDG paints on LCD, not inside this LVGL child). */
#define KARAOKE_CDG_FRAME_PAD    5
#define KARAOKE_CDG_FRAME_BORDER 3
#define KARAOKE_CDG_FRAME_RADIUS 12

static void dashcdg_ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void karaoke_status_fill_bat(char *bat_txt, size_t bat_sz, lv_color_t *bat_col)
{
    int raw = 0;
    int vbat = 0;
    esp_err_t br = ESP_FAIL;
    if (dashcdg_platform_hw_is_ready()) {
        br = dashcdg_platform_hw_battery_read(&raw, NULL, &vbat);
    }
    if (br != ESP_OK) {
        if (!dashcdg_vbat_sense_is_ready() || dashcdg_vbat_sense_read(&raw, NULL, &vbat) != ESP_OK) {
            snprintf(bat_txt, bat_sz, LV_SYMBOL_BATTERY_EMPTY " --");
            *bat_col = lv_color_hex(0x887766);
            return;
        }
    }
    dashcdg_battery_format_status_line_raw(bat_txt, bat_sz, vbat, raw);
    *bat_col = dashcdg_battery_label_color_from_pack_mv(vbat);
}

static void karaoke_status_fill_wifi(char *wifi_txt, size_t wifi_sz)
{
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);
    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " off");
    } else if (werr == ESP_OK) {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " %d", ap.rssi);
    } else {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " --");
    }
}

static void karaoke_status_bar_set_pill(lv_obj_t *lbl, lv_color_t col)
{
    lv_obj_set_style_text_color(lbl, col, 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(0x0a1210), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(lbl, 5, 0);
    lv_obj_set_style_pad_ver(lbl, 1, 0);
    lv_obj_set_style_radius(lbl, 3, 0);
    lv_obj_set_style_border_width(lbl, 1, 0);
    lv_obj_set_style_border_color(lbl, col, 0);
}

static bool karaoke_status_bar_widgets_ok(void)
{
    return s_bar_wifi && lv_obj_is_valid(s_bar_wifi) && s_bar_bat && lv_obj_is_valid(s_bar_bat) && s_bar_m &&
           lv_obj_is_valid(s_bar_m) && s_bar_c && lv_obj_is_valid(s_bar_c) && s_bar_d && lv_obj_is_valid(s_bar_d) &&
           s_bar_line && lv_obj_is_valid(s_bar_line);
}

static void karaoke_status_bar_update_slow(void)
{
    if (!karaoke_status_bar_widgets_ok()) {
        return;
    }

    char wbuf[40];
    karaoke_status_fill_wifi(wbuf, sizeof(wbuf));
    lv_label_set_text(s_bar_wifi, wbuf);

    char bbuf[40];
    lv_color_t bcol;
    karaoke_status_fill_bat(bbuf, sizeof(bbuf), &bcol);
    lv_label_set_text(s_bar_bat, bbuf);
    lv_obj_set_style_text_color(s_bar_bat, bcol, 0);
}

static void karaoke_status_bar_update_fast(const dashcdg_badge_rx_stats_t *st)
{
    if (!karaoke_status_bar_widgets_ok()) {
        return;
    }

    const lv_color_t okc = lv_color_hex(0x44ee99);
    const lv_color_t waitc = lv_color_hex(0xddbb44);
    const lv_color_t idlec = lv_color_hex(0x556677);
    const lv_color_t badc = lv_color_hex(0xff8866);

    const bool rx_on = (st->rx_task_running != 0) ||
                       (st->datagrams > 0U) ||
                       (st->v4_clock_count > 0U) ||
                       (st->v4_video_delta_count > 0U) ||
                       (st->parse_failures > 0U) ||
                       (st->cdg_missing_estimate > 0U);
    const bool joined = (st->igmp_joined != 0) || (st->datagrams > 0U) || (st->v4_clock_count > 0U);
    const bool clock = (st->have_clock != 0) || (st->v4_clock_count > 0U);
    uint64_t now_ms = dashcdg_clock_now_ms();
    static uint64_t s_prev_datagrams;
    static uint32_t s_prev_clock_count;
    static uint32_t s_prev_delta_count;
    uint32_t miss_per_10k = 0U;
    uint32_t reorder_per_10k = 0U;
    bool buf_recent;

    if (st->datagrams > s_prev_datagrams) {
        s_last_m_progress_ms = now_ms;
    }
    if (st->v4_clock_count > s_prev_clock_count) {
        s_last_c_progress_ms = now_ms;
    }
    if (st->v4_video_delta_count > s_prev_delta_count) {
        s_last_d_progress_ms = now_ms;
    }
    s_prev_datagrams = st->datagrams;
    s_prev_clock_count = st->v4_clock_count;
    s_prev_delta_count = st->v4_video_delta_count;

    if (s_last_q_sample_ms == 0U) {
        s_last_q_sample_ms = now_ms;
        s_last_q_datagrams = st->datagrams;
        s_last_q_wire_miss = st->wire_missing_estimate;
        s_last_q_cdg_miss = st->cdg_missing_estimate;
        s_last_q_wire_reorder = st->wire_reorder_events;
        s_last_q_parse_fail = st->parse_failures;
        s_last_q_repair_fail = st->v4_video_repair_failed;
    } else if ((now_ms - s_last_q_sample_ms) >= 2000U &&
            st->datagrams >= s_last_q_datagrams &&
            st->wire_missing_estimate >= s_last_q_wire_miss &&
            st->cdg_missing_estimate >= s_last_q_cdg_miss &&
            st->wire_reorder_events >= s_last_q_wire_reorder &&
            st->parse_failures >= s_last_q_parse_fail &&
            st->v4_video_repair_failed >= s_last_q_repair_fail &&
            st->datagrams > s_last_q_datagrams) {
        uint64_t dg = st->datagrams - s_last_q_datagrams;
        uint64_t miss = st->wire_missing_estimate - s_last_q_wire_miss;
        uint32_t cdg_miss = st->cdg_missing_estimate - s_last_q_cdg_miss;
        uint32_t reord = st->wire_reorder_events - s_last_q_wire_reorder;
        uint32_t parse_fail = st->parse_failures - s_last_q_parse_fail;
        uint32_t repair_fail = st->v4_video_repair_failed - s_last_q_repair_fail;
        uint32_t decode_bad_per_10k = (uint32_t)((((uint64_t)parse_fail + (uint64_t)repair_fail) * 10000ULL) / dg);

        miss_per_10k = (uint32_t)((miss * 10000ULL) / dg);
        {
            uint32_t cdg_miss_per_10k = (uint32_t)(((uint64_t)cdg_miss * 10000ULL) / dg);
            if (cdg_miss_per_10k > miss_per_10k) {
                miss_per_10k = cdg_miss_per_10k;
            }
        }
        if (decode_bad_per_10k > miss_per_10k) {
            miss_per_10k = decode_bad_per_10k;
        }
        reorder_per_10k = (uint32_t)(((uint64_t)reord * 10000ULL) / dg);
        if (miss_per_10k > 10000U) {
            miss_per_10k = 10000U;
        }
        if (reorder_per_10k > 10000U) {
            reorder_per_10k = 10000U;
        }
        s_last_q_miss_per_10k = miss_per_10k;
        s_last_q_reorder_per_10k = reorder_per_10k;
        if (s_loss_hist_count >= KARAOKE_LOSS_AVG_SAMPLES) {
            uint8_t old_idx = s_loss_hist_head;
            s_loss_hist_sum_per_10k -= s_loss_hist_per_10k[old_idx];
        } else {
            s_loss_hist_count++;
        }
        s_loss_hist_per_10k[s_loss_hist_head] = miss_per_10k;
        s_loss_hist_sum_per_10k += miss_per_10k;
        s_loss_hist_head = (uint8_t)((s_loss_hist_head + 1U) % KARAOKE_LOSS_AVG_SAMPLES);
        s_last_q_sample_ms = now_ms;
        s_last_q_datagrams = st->datagrams;
        s_last_q_wire_miss = st->wire_missing_estimate;
        s_last_q_cdg_miss = st->cdg_missing_estimate;
        s_last_q_wire_reorder = st->wire_reorder_events;
        s_last_q_parse_fail = st->parse_failures;
        s_last_q_repair_fail = st->v4_video_repair_failed;
    } else if ((now_ms - s_last_q_sample_ms) >= 2000U) {
        /* Counter reset/restart or no progress: re-baseline without producing a bogus spike. */
        s_last_q_sample_ms = now_ms;
        s_last_q_datagrams = st->datagrams;
        s_last_q_wire_miss = st->wire_missing_estimate;
        s_last_q_cdg_miss = st->cdg_missing_estimate;
        s_last_q_wire_reorder = st->wire_reorder_events;
        s_last_q_parse_fail = st->parse_failures;
        s_last_q_repair_fail = st->v4_video_repair_failed;
    }
    if (miss_per_10k == 0U && reorder_per_10k == 0U) {
        miss_per_10k = s_last_q_miss_per_10k;
        reorder_per_10k = s_last_q_reorder_per_10k;
    }
    /* Reorder-heavy periods often look like corruption even before strict missing estimate rises. */
    if (miss_per_10k == 0U && reorder_per_10k > 0U) {
        miss_per_10k = reorder_per_10k / 2U;
    }
    if (miss_per_10k == 0U && st->datagrams > 0U &&
            (st->cdg_missing_estimate > 0U || st->parse_failures > 0U || st->v4_video_repair_failed > 0U)) {
        uint64_t derived = (((uint64_t)st->cdg_missing_estimate + (uint64_t)st->parse_failures +
                            (uint64_t)st->v4_video_repair_failed) * 10000ULL) / st->datagrams;
        if (derived > 10000ULL) {
            derived = 10000ULL;
        }
        miss_per_10k = (uint32_t)derived;
    }

    if (st->jb_pending_slots > 0U || st->audio_jb_pending_slots > 0U) {
        s_last_buf_nonzero_ms = now_ms;
    }
    buf_recent = (s_last_buf_nonzero_ms != 0U) && ((now_ms - s_last_buf_nonzero_ms) <= 1800U);
    const bool buf_ok =
            (st->video_decode_enabled != 0U)
                    ? (((st->jb_pending_slots > 0U) || buf_recent) && (st->v4_video_delta_count > 0U))
                    : ((st->audio_decode_enabled != 0U) &&
                       ((st->audio_jb_pending_slots > 0U) || buf_recent || st->v4_audio_frames_out > 0U ||
                        st->v4_audio_chunk_rx > 0U));
    const bool stream_ok =
            clock && buf_ok && ((st->video_decode_enabled == 0U) || (st->cdg_heap_ok != 0));

    lv_color_t mc = idlec;
    if (!rx_on) {
        mc = idlec;
    } else if (!joined) {
        mc = waitc;
    } else if (miss_per_10k > 260U) {
        mc = badc;
    } else if (st->datagrams > 0U) {
        mc = okc;
    } else {
        mc = waitc;
    }
    karaoke_status_bar_set_pill(s_bar_m, mc);
    lv_label_set_text(s_bar_m, "M");

    lv_color_t cc = idlec;
    if (!rx_on) {
        cc = idlec;
    } else if (!joined) {
        cc = idlec;
    } else if (!clock) {
        cc = waitc;
    } else if (miss_per_10k > 320U || (st->skew_ema_ms > 1500 || st->skew_ema_ms < -1500)) {
        cc = badc;
    } else if (st->v4_clock_count > 0U) {
        cc = okc;
    } else {
        cc = waitc;
    }
    karaoke_status_bar_set_pill(s_bar_c, cc);
    lv_label_set_text(s_bar_c, "C");

    lv_color_t dc = idlec;
    if (!rx_on || !joined || !clock) {
        dc = idlec;
    } else if (miss_per_10k > 300U || reorder_per_10k > 450U) {
        dc = badc;
    } else if (stream_ok) {
        dc = okc;
    } else if ((st->video_decode_enabled != 0U) && st->cdg_heap_ok == 0) {
        dc = badc;
    } else {
        dc = waitc;
    }
    karaoke_status_bar_set_pill(s_bar_d, dc);
    lv_label_set_text(s_bar_d, "D");

    const char *sym = LV_SYMBOL_REFRESH;
    lv_color_t lcol = waitc;
    if (!rx_on) {
        sym = LV_SYMBOL_PAUSE;
        lcol = idlec;
    } else if (!joined) {
        sym = LV_SYMBOL_REFRESH;
        lcol = waitc;
    } else if (!clock) {
        sym = LV_SYMBOL_REFRESH;
        lcol = waitc;
    } else if ((st->video_decode_enabled != 0U) && st->cdg_heap_ok == 0) {
        sym = LV_SYMBOL_CLOSE;
        lcol = badc;
    } else if (miss_per_10k > 300U) {
        sym = LV_SYMBOL_WARNING;
        lcol = badc;
    } else if (miss_per_10k > 80U || !buf_ok) {
        sym = LV_SYMBOL_REFRESH;
        lcol = waitc;
    } else {
        sym = LV_SYMBOL_OK;
        lcol = okc;
    }
    char line[72];
    if (!rx_on || !joined) {
        snprintf(line, sizeof(line), "%s --.-%% loss", sym);
    } else {
        uint32_t loss_for_display = miss_per_10k;
        unsigned ap = 0U;
        unsigned vp = 0U;
        if (s_loss_hist_count > 0U) {
            loss_for_display = s_loss_hist_sum_per_10k / (uint32_t)s_loss_hist_count;
        }
        if (loss_for_display > 10000U) {
            loss_for_display = 10000U;
        }
        uint32_t loss_tenths = (loss_for_display + 5U) / 10U; /* per-10k to percent with one decimal */
        if (st->audio_slots_capacity > 0U) {
            ap = (unsigned)((st->audio_jb_pending_slots * 100U) / (uint32_t)st->audio_slots_capacity);
        }
        if (st->video_slots_capacity > 0U) {
            vp = (unsigned)(((uint32_t)st->jb_pending_slots * 100U) / (uint32_t)st->video_slots_capacity);
        }
        snprintf(line, sizeof(line), "%s %u.%u%% a%u%% v%u%%", sym, (unsigned)(loss_tenths / 10U),
                 (unsigned)(loss_tenths % 10U), ap, vp);
    }
    karaoke_status_bar_set_pill(s_bar_line, lcol);
    lv_label_set_text(s_bar_line, line);
    lv_obj_set_style_text_color(s_bar_line, lcol, 0);
}

static void mcast_modal_cancel_pending_close(void)
{
    if (s_mcast_modal_close_timer != NULL) {
        lv_timer_del(s_mcast_modal_close_timer);
        s_mcast_modal_close_timer = NULL;
    }
    if (s_mcast_modal_close_root != NULL) {
        if (lv_obj_is_valid(s_mcast_modal_close_root)) {
            lv_obj_del(s_mcast_modal_close_root);
        }
        s_mcast_modal_close_root = NULL;
    }
}

static void mcast_modal_close_timer_cb(lv_timer_t *t)
{
    lv_obj_t *r = s_mcast_modal_close_root;

    s_mcast_modal_close_timer = NULL;
    s_mcast_modal_close_root = NULL;
    lv_timer_del(t);
    if (r != NULL && lv_obj_is_valid(r)) {
        lv_obj_del(r);
    }
    if (s_cdg_slot != NULL && lv_obj_is_valid(s_cdg_slot)) {
        lv_obj_invalidate(s_cdg_slot);
        lv_obj_t *fr = lv_obj_get_parent(s_cdg_slot);
        if (fr != NULL && lv_obj_is_valid(fr)) {
            lv_obj_invalidate(fr);
        }
    }
}

/** Close dashboard from a button/input callback: defer delete to the next LVGL timer tick. */
static void mcast_modal_close_deferred(void)
{
    lv_obj_t *root = s_mcast_modal_root;

    if (root == NULL) {
        return;
    }
    mcast_modal_cancel_pending_close();
    s_mcast_modal_root = NULL;
    s_mcast_card_net = NULL;
    s_mcast_card_stream = NULL;
    s_mcast_card_repair = NULL;
    s_mcast_card_memory = NULL;
    s_mcast_card_system = NULL;
    s_mcast_card_song = NULL;
    s_mcast_modal_body_ticks = 0U;
    s_mcast_modal_close_root = root;
    s_mcast_modal_close_timer = lv_timer_create(mcast_modal_close_timer_cb, 1, NULL);
}

/** Synchronous close (teardown / navigation): cancel any deferred delete then remove modal. */
static void mcast_modal_close_sync(void)
{
    lv_obj_t *root;

    mcast_modal_cancel_pending_close();
    root = s_mcast_modal_root;
    s_mcast_modal_root = NULL;
    s_mcast_card_net = NULL;
    s_mcast_card_stream = NULL;
    s_mcast_card_repair = NULL;
    s_mcast_card_memory = NULL;
    s_mcast_card_system = NULL;
    s_mcast_card_song = NULL;
    s_mcast_modal_body_ticks = 0U;
    if (root != NULL && lv_obj_is_valid(root)) {
        lv_obj_del(root);
    }
    if (s_cdg_slot != NULL && lv_obj_is_valid(s_cdg_slot)) {
        lv_obj_invalidate(s_cdg_slot);
        lv_obj_t *fr = lv_obj_get_parent(s_cdg_slot);
        if (fr != NULL && lv_obj_is_valid(fr)) {
            lv_obj_invalidate(fr);
        }
    }
}

/** CDG is drawn with draw_bitmap on the panel; while the modal exists it must not run or it paints over the dialog. */
static bool karaoke_mcast_modal_is_open(void)
{
    if (s_mcast_modal_root != NULL && lv_obj_is_valid(s_mcast_modal_root)) {
        return true;
    }
    /* Deferred close: root still on-screen until one-shot timer runs. */
    if (s_mcast_modal_close_root != NULL && lv_obj_is_valid(s_mcast_modal_close_root)) {
        return true;
    }
    return false;
}

void dashcdg_karaoke_ui_teardown(void)
{
    dashcdg_platform_hw_set_cdg_stream_ok(false);
    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_HOME);
    if (lvgl_port_lock(1000)) {
        mcast_modal_close_sync();
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
    s_cdg_frame = NULL;
    s_stage_fill = NULL;
    s_audio_only_panel = NULL;
    s_audio_track_lbl = NULL;
    s_audio_vol_slider = NULL;
    s_audio_vol_pct_lbl = NULL;
    s_audio_mute_btn = NULL;
    s_audio_mute_lbl = NULL;
    s_was_audio_only_mode = false;
    s_bar_wifi = NULL;
    s_bar_bat = NULL;
    s_bar_m = NULL;
    s_bar_c = NULL;
    s_bar_d = NULL;
    s_bar_line = NULL;
    s_last_buf_nonzero_ms = 0U;
    s_last_m_progress_ms = 0U;
    s_last_c_progress_ms = 0U;
    s_last_d_progress_ms = 0U;
    s_last_q_sample_ms = 0U;
    s_last_q_datagrams = 0U;
    s_last_q_wire_miss = 0U;
    s_last_q_cdg_miss = 0U;
    s_last_q_wire_reorder = 0U;
    s_last_q_parse_fail = 0U;
    s_last_q_repair_fail = 0U;
    s_last_q_miss_per_10k = 0U;
    s_last_q_reorder_per_10k = 0U;
    memset(s_loss_hist_per_10k, 0, sizeof(s_loss_hist_per_10k));
    s_loss_hist_count = 0U;
    s_loss_hist_head = 0U;
    s_loss_hist_sum_per_10k = 0U;
    s_status_slow_deadline_ms = 0U;
}

static void on_mcast_ok(lv_event_t *e)
{
    (void)e;
    mcast_modal_close_deferred();
}

static lv_obj_t *karaoke_mcast_dashboard_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0b1320), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2f7cff), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 9, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x2266dd), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    dashcdg_ui_no_scroll(card);

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x8fd8ff), 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(0xd7f3ff), 0);
    lv_label_set_text(body, "--");
    return body;
}

static void karaoke_mcast_modal_update_dashboard(void)
{
    dashcdg_badge_rx_stats_t st;
    char line[288];
    unsigned loss_x100 = 0U;
    unsigned long heap_free;
    unsigned long heap_min;
    unsigned long int_largest;

    if ((s_mcast_dash_heap_refresh_i++ % 4U) == 0U) {
        s_mcast_dash_heap_free = (unsigned long)esp_get_free_heap_size();
        s_mcast_dash_heap_min = (unsigned long)esp_get_minimum_free_heap_size();
        s_mcast_dash_int_largest =
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    heap_free = s_mcast_dash_heap_free;
    heap_min = s_mcast_dash_heap_min;
    int_largest = s_mcast_dash_int_largest;
    unsigned audio_occ_pct = 0U;
    unsigned video_occ_pct = 0U;
    const char *profile = "bal";

    if (!(s_mcast_card_net && s_mcast_card_stream && s_mcast_card_repair && s_mcast_card_memory && s_mcast_card_system &&
          s_mcast_card_song)) {
        return;
    }
    dashcdg_badge_rx_get_stats(&st);
    if (st.datagrams > 0U) {
        uint64_t lx100 = (st.wire_missing_estimate * 10000ULL) / st.datagrams;
        if (lx100 > 10000ULL) {
            lx100 = 10000ULL;
        }
        loss_x100 = (unsigned)lx100;
    }
    if (st.audio_slots_capacity > 0U) {
        audio_occ_pct = (unsigned)((st.audio_jb_pending_slots * 100U) / (uint32_t)st.audio_slots_capacity);
    }
    if (st.video_slots_capacity > 0U) {
        video_occ_pct = (unsigned)(((uint32_t)st.jb_pending_slots * 100U) / (uint32_t)st.video_slots_capacity);
    }
    switch (st.memory_profile) {
    case 0:
        profile = "min";
        break;
    case 1:
        profile = "aud";
        break;
    case 2:
        profile = "vid";
        break;
    default:
        profile = "bal";
        break;
    }

    snprintf(line, sizeof(line), "MCAST %s\nSTA %s\nIGMP %s\nuc m/s/r %c/%c/%c\nstats tx/peer %lu/%lu",
             st.tx_stats_dest[0] ? st.tx_stats_dest : "--", st.sta_ip[0] ? st.sta_ip : "--",
             st.igmp_joined ? "joined" : "no",
             (st.ucast_rx_mask & DASHCDG_BADGE_UCAST_RX_MASK_MEDIA) ? 'y' : '-',
             (st.ucast_rx_mask & DASHCDG_BADGE_UCAST_RX_MASK_STATS) ? 'y' : '-',
             (st.ucast_rx_mask & DASHCDG_BADGE_UCAST_RX_MASK_REPAIR) ? 'y' : '-',
             (unsigned long)st.v4_rx_stats_sent, (unsigned long)st.v4_rx_stats_peer_packets);
    lv_label_set_text(s_mcast_card_net, line);

    snprintf(line, sizeof(line), "loss %u.%02u%% reorder %lu\nclk %lu d %lu anch %lu rej %lu\naudio rx/out %lu/%lu",
             (unsigned)(loss_x100 / 100U), (unsigned)(loss_x100 % 100U), (unsigned long)st.wire_reorder_events,
             (unsigned long)st.v4_clock_count, (unsigned long)st.v4_video_delta_count, (unsigned long)st.v4_anchor_chunks,
             (unsigned long)st.v4_anchor_rejected_behind, (unsigned long)st.v4_audio_chunk_rx,
             (unsigned long)st.v4_audio_frames_out);
    lv_label_set_text(s_mcast_card_stream, line);

    snprintf(line, sizeof(line), "ctl tx %s  fec %s\nnack ok %lu att %lu fail %lu thr %lu req %s\nrepair f/r %lu/%lu\nok/fail %lu/%lu\nmiss %lu",
             st.v4_control_uplink_ok ? "ded" : "24685rx", st.v4_repair_rx_socket_ok ? "24686 ok" : "24686 off",
             (unsigned long)st.v4_repair_nack_tx,
             (unsigned long)st.v4_repair_nack_attempt, (unsigned long)st.v4_repair_nack_send_fail,
             (unsigned long)st.v4_repair_nack_throttled,
             st.repair_nack_enabled ? "on" : "off", (unsigned long)st.v4_video_repair_rx_forward,
             (unsigned long)st.v4_video_repair_rx_reverse,
             (unsigned long)st.v4_video_repair_recovered, (unsigned long)st.v4_video_repair_failed,
             (unsigned long)st.repair_missing_estimate);
    lv_label_set_text(s_mcast_card_repair, line);

    snprintf(line, sizeof(line), "profile %s sw %lu\nslots cap a/v %u/%u\njb fill a/v %u%%/%u%% pend %u/%zu\nevict %lu rsz fail %lu\nstats tx %s",
             profile, (unsigned long)st.memory_profile_switches, (unsigned)st.audio_slots_capacity,
             (unsigned)st.video_slots_capacity, audio_occ_pct, video_occ_pct, (unsigned)st.audio_jb_pending_slots,
             st.jb_pending_slots, (unsigned long)st.jb_evict_rounds, (unsigned long)st.memory_profile_resize_failures,
             st.v4_stats_tx_enabled ? "on" : "off");
    lv_label_set_text(s_mcast_card_memory, line);

    snprintf(line, sizeof(line), "heap free/min %lu/%lu\nint largest %lu\nparse fail %lu\nseq %llu",
             heap_free, heap_min, int_largest, (unsigned long)st.parse_failures, (unsigned long long)st.last_sequence);
    lv_label_set_text(s_mcast_card_system, line);

    snprintf(line, sizeof(line), "%s\nclock %s  decode %s\ndec %lu deg %lu jb %lu dac %lu\ncdg hr %lu/%lu lag %ld",
             st.song_id[0] ? st.song_id : "(none)", st.have_clock ? "yes" : "no", st.cdg_heap_ok ? "on" : "off",
             (unsigned long)st.v4_audio_decode_fail, (unsigned long)st.v4_audio_degraded_push,
             (unsigned long)st.v4_audio_jitter_skip_events, (unsigned long)st.v4_audio_dac_begin_fail,
             (unsigned long)st.cdg_hard_resync_events, (unsigned long)st.cdg_hard_resync_packets, (long)st.cdg_lag_ms);
    lv_label_set_text(s_mcast_card_song, line);
}

static void on_info_btn(lv_event_t *e)
{
    (void)e;
    if (karaoke_mcast_modal_is_open()) {
        return;
    }
    mcast_modal_cancel_pending_close();

    lv_obj_t *layer = lv_layer_top();
    lv_obj_t *root = lv_obj_create(layer);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x03060f), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 8, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_mcast_modal_root = root;

    lv_obj_t *hdr = lv_obj_create(root);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x081427), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x2f7cff), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_radius(hdr, 8, 0);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(hdr);

    lv_obj_t *mtitle = lv_label_create(hdr);
    lv_label_set_text(mtitle, LV_SYMBOL_EYE_OPEN " CDG RX LIVE DASHBOARD");
    lv_obj_set_style_text_color(mtitle, lv_color_hex(0x8fd8ff), 0);

    lv_obj_t *b = lv_button_create(hdr);
    lv_obj_set_width(b, 92);
    lv_obj_set_height(b, 32);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x132944), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x6ab6ff), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE " close");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xd7f3ff), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_mcast_ok, LV_EVENT_CLICKED, NULL);

    lv_obj_t *board = lv_obj_create(root);
    lv_obj_set_width(board, lv_pct(100));
    lv_obj_set_flex_grow(board, 1);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x04101f), 0);
    lv_obj_set_style_radius(board, 8, 0);
    lv_obj_set_style_pad_all(board, 6, 0);
    lv_obj_set_style_pad_row(board, 7, 0);
    lv_obj_set_style_pad_right(board, 12, 0);
    lv_obj_set_flex_flow(board, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(board, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(board, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(board, LV_DIR_VER);
    lv_obj_add_flag(board, LV_OBJ_FLAG_SCROLLABLE);

    s_mcast_card_net = karaoke_mcast_dashboard_card(board, "NET");
    s_mcast_card_stream = karaoke_mcast_dashboard_card(board, "STREAM");
    s_mcast_card_repair = karaoke_mcast_dashboard_card(board, "REPAIR");
    s_mcast_card_memory = karaoke_mcast_dashboard_card(board, "MEMORY");
    s_mcast_card_system = karaoke_mcast_dashboard_card(board, "SYSTEM");
    s_mcast_card_song = karaoke_mcast_dashboard_card(board, "SONG");
    karaoke_mcast_modal_update_dashboard();
    lv_obj_update_layout(root);
    lv_obj_move_foreground(root);
}

static void karaoke_audio_refresh_vol_widgets(void)
{
    uint8_t pct = dashcdg_platform_hw_get_karaoke_output_volume_pct();

    if (s_audio_vol_slider != NULL && lv_obj_is_valid(s_audio_vol_slider)) {
        s_audio_vol_slider_guard = true;
        lv_slider_set_value(s_audio_vol_slider, (int32_t)pct, LV_ANIM_OFF);
        s_audio_vol_slider_guard = false;
    }
    if (s_audio_vol_pct_lbl != NULL && lv_obj_is_valid(s_audio_vol_pct_lbl)) {
        char b[12];

        snprintf(b, sizeof(b), "%u%%", (unsigned)pct);
        lv_label_set_text(s_audio_vol_pct_lbl, b);
    }
    if (s_audio_mute_lbl != NULL && lv_obj_is_valid(s_audio_mute_lbl)) {
        if (pct == 0U) {
            lv_label_set_text(s_audio_mute_lbl, LV_SYMBOL_VOLUME_MAX "  Unmute");
        } else {
            lv_label_set_text(s_audio_mute_lbl, LV_SYMBOL_MUTE "  Mute");
        }
    }
}

/*
 * `s_audio_only_panel` and `s_stage_fill` both used flex_grow=1. HIDDEN alone still participates in
 * flex on some LVGL layouts — the strip stole ~half the stage below CDG while video was on / audio
 * decode off, leaving a tall blank band over the lyrics area.
 */
static void karaoke_stage_apply_audio_only_vs_video_layout(bool audio_only_karaoke)
{
    if (s_audio_only_panel != NULL && lv_obj_is_valid(s_audio_only_panel)) {
        if (audio_only_karaoke) {
            lv_obj_set_flex_grow(s_audio_only_panel, 1);
            lv_obj_set_height(s_audio_only_panel, LV_SIZE_CONTENT);
            lv_obj_remove_flag(s_audio_only_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_flex_grow(s_audio_only_panel, 0);
            lv_obj_set_height(s_audio_only_panel, 0);
            lv_obj_add_flag(s_audio_only_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_cdg_frame != NULL && lv_obj_is_valid(s_cdg_frame)) {
        if (audio_only_karaoke) {
            lv_obj_add_flag(s_cdg_frame, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_cdg_frame, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_stage_fill != NULL && lv_obj_is_valid(s_stage_fill)) {
        if (audio_only_karaoke) {
            lv_obj_set_flex_grow(s_stage_fill, 0);
            lv_obj_set_height(s_stage_fill, 0);
            lv_obj_add_flag(s_stage_fill, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_flex_grow(s_stage_fill, 1);
            lv_obj_set_height(s_stage_fill, LV_SIZE_CONTENT);
            lv_obj_remove_flag(s_stage_fill, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_audio_vol_slider(lv_event_t *e)
{
    if (s_audio_vol_slider_guard) {
        return;
    }
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);

    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    {
        uint8_t pct = (uint8_t)v;

        dashcdg_platform_hw_set_karaoke_output_volume_pct(pct);
        dashcdg_badge_prefs_schedule_karaoke_output_volume_save(pct, dashcdg_clock_now_ms());
    }
    karaoke_audio_refresh_vol_widgets();
}

static void on_audio_mute_btn(lv_event_t *e)
{
    (void)e;
    uint8_t cur = dashcdg_platform_hw_get_karaoke_output_volume_pct();
    uint64_t now = dashcdg_clock_now_ms();

    if (cur > 0U) {
        s_audio_vol_before_mute = cur;
        dashcdg_platform_hw_set_karaoke_output_volume_pct(0);
        dashcdg_badge_prefs_schedule_karaoke_output_volume_save(0, now);
    } else {
        uint8_t r = s_audio_vol_before_mute;

        if (r == 0U) {
            r = 85U;
        }
        dashcdg_platform_hw_set_karaoke_output_volume_pct(r);
        dashcdg_badge_prefs_schedule_karaoke_output_volume_save(r, now);
    }
    karaoke_audio_refresh_vol_widgets();
}

static void karaoke_ui_decode_layout_prefs_reload(void)
{
    uint8_t pv = 1U;
    uint8_t pa = 1U;

    (void)dashcdg_badge_prefs_load_karaoke_video_decode(&pv);
    (void)dashcdg_badge_prefs_load_karaoke_audio_decode(&pa);
    s_kui_layout_pref_video = (pv != 0U) ? 1U : 0U;
    s_kui_layout_pref_audio = (pa != 0U) ? 1U : 0U;
}

static void karaoke_ui_apply_stage_from_cached_decode_prefs(const dashcdg_badge_rx_stats_t *st)
{
    bool audio_only = (s_kui_layout_pref_video == 0U && s_kui_layout_pref_audio != 0U);

    if (audio_only != s_was_audio_only_mode) {
        s_was_audio_only_mode = audio_only;
        if (audio_only) {
            karaoke_audio_refresh_vol_widgets();
        }
    }
    if (audio_only) {
        karaoke_stage_apply_audio_only_vs_video_layout(true);
        if (s_audio_track_lbl && lv_obj_is_valid(s_audio_track_lbl) && st != NULL) {
            static char prev_track[88];

            if (strncmp(prev_track, st->song_id, sizeof(prev_track)) != 0) {
                strncpy(prev_track, st->song_id, sizeof(prev_track) - 1U);
                prev_track[sizeof(prev_track) - 1U] = '\0';
                if (st->song_id[0] == '\0') {
                    lv_label_set_text(s_audio_track_lbl, "Track: (waiting)");
                } else {
                    char buf[128];

                    snprintf(buf, sizeof(buf), "Track:\n%s", st->song_id);
                    lv_label_set_text(s_audio_track_lbl, buf);
                }
            }
        }
    } else {
        karaoke_stage_apply_audio_only_vs_video_layout(false);
    }
}

void dashcdg_karaoke_ui_sync_decode_layout_from_prefs(void)
{
    /*
     * Decode toggles are written from settings UI; NV is the source of truth for stage layout so we
     * do not depend on RX mutex latency. Avoid LVGL work when karaoke is not on-screen.
     */
    if (s_tick == NULL) {
        karaoke_ui_decode_layout_prefs_reload();
        return;
    }
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "sync decode layout: LVGL lock timeout");
        return;
    }
    karaoke_ui_decode_layout_prefs_reload();
    {
        dashcdg_badge_rx_stats_t st;

        dashcdg_badge_rx_get_stats(&st);
        karaoke_ui_apply_stage_from_cached_decode_prefs(&st);
    }
    lvgl_port_unlock();
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
        if (++s_heap_retry_ticks >= 120U) {
            s_heap_retry_ticks = 0U;
            dashcdg_badge_rx_try_upgrade_cdg_heap();
        }
        /* PM: CDG blit cadence proves the UI is alive even if jitter pending hits 0 between frames. */
        dashcdg_platform_hw_note_karaoke_cdg_overlay_tick(dashcdg_clock_now_ms());
        {
            const uint64_t now = dashcdg_clock_now_ms();
            dashcdg_badge_rx_stats_t st;

            /*
             * Decode layout NVS is not read here — only via prefs_poll (debounced save) and explicit
             * sync when settings apply (KAR-03 / T10).
             */
            dashcdg_badge_prefs_poll_karaoke_output_volume_save(now);
            if (!karaoke_mcast_modal_is_open()) {
                dashcdg_badge_rx_lvgl_overlay_tick_and_get_stats(s_cdg_slot, &st);
            } else {
                dashcdg_badge_rx_get_stats(&st);
            }
            /*
             * Stream OK for PM / backlight: video needs CDG jitter progress; audio-only needs chunks/frames.
             */
            bool ok = false;
            if (st.have_clock != 0) {
                if (st.video_decode_enabled != 0U) {
                    ok = (st.jb_pending_slots > 0U || st.v4_video_delta_count > 0U);
                } else if (st.audio_decode_enabled != 0U) {
                    ok = (st.v4_audio_chunk_rx > 0U || st.v4_audio_frames_out > 0U || st.audio_jb_pending_slots > 0U);
                }
            }
            dashcdg_platform_hw_set_cdg_stream_ok(ok);
            if (s_status_slow_deadline_ms == 0U || now >= s_status_slow_deadline_ms) {
                karaoke_status_bar_update_slow();
                s_status_slow_deadline_ms = now + (uint64_t)KARAOKE_STATUS_SLOW_PERIOD_MS;
            }
            karaoke_status_bar_update_fast(&st);

            karaoke_ui_apply_stage_from_cached_decode_prefs(&st);
        }
        if (karaoke_mcast_modal_is_open()) {
            if (++s_mcast_modal_body_ticks >= 32U) {
                s_mcast_modal_body_ticks = 0U;
                karaoke_mcast_modal_update_dashboard();
            }
        } else {
            s_mcast_modal_body_ticks = 0U;
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
    ESP_LOGD(
            TAG,
            "heap after screen clear (before RX): internal_free=%u internal_largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /*
     * Multicast + IGMP must run with the STA out of modem PS (home uses WIFI_PS_MAX_MODEM). Screen
     * used to switch to KARAOKE only after the LVGL tree was built, so the first join/bind raced PS —
     * cold boot often saw zero packets until reset. Enter KARAOKE (PS_NONE) before sockets exist.
     */
    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_KARAOKE);
    /*
     * Start RX (and try CDG/jitter calloc) while the screen is still empty so internal heap is not
     * fragmented by the karaoke LVGL tree yet.
     */
    s_heap_retry_ticks = 0U;
    dashcdg_badge_rx_start();
    if (!lvgl_port_lock(1000)) {
        dashcdg_badge_rx_stop();
        dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_HOME);
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    /*
     * Vertical budget: 240 ~= header(22) + stage (rest). Status is the flex-grow middle of the
     * header row (back | status | ?). CDG slot is the first child of the stage so it sits under
     * the header with spacer below, not a large empty band above the raster.
     */
    lv_obj_set_style_pad_hor(outer, 6, 0);
    lv_obj_set_style_pad_top(outer, 0, 0);
    lv_obj_set_style_pad_bottom(outer, 0, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(outer, 0, 0);
    dashcdg_ui_no_scroll(outer);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 22);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(top);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_size(b_back, 26, 22);
    lv_obj_set_style_pad_all(b_back, 0, 0);
    lv_obj_set_style_radius(b_back, 3, 0);
    lv_obj_set_style_bg_color(b_back, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b_back, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b_back, 1, 0);
    lv_obj_set_style_shadow_width(b_back, 0, 0);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x66ffcc), 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    /* One flex-grow strip between fixed buttons: keeps "?" on-screen (content-sized middle + two
     * spacers was wider than 320 - pad and pushed b_info off). */
    lv_obj_t *row_stat = lv_obj_create(top);
    lv_obj_set_height(row_stat, 22);
    lv_obj_set_flex_grow(row_stat, 1);
    lv_obj_set_style_min_width(row_stat, 0, 0);
    lv_obj_set_style_pad_all(row_stat, 0, 0);
    lv_obj_set_style_border_width(row_stat, 0, 0);
    lv_obj_set_style_bg_opa(row_stat, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row_stat, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_stat, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_stat, 3, 0);
    dashcdg_ui_no_scroll(row_stat);

    s_bar_wifi = lv_label_create(row_stat);
    lv_label_set_long_mode(s_bar_wifi, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(s_bar_wifi, lv_color_hex(0xaaccdd), 0);
    lv_label_set_text(s_bar_wifi, LV_SYMBOL_WIFI " --");

    s_bar_bat = lv_label_create(row_stat);
    lv_label_set_long_mode(s_bar_bat, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(s_bar_bat, lv_color_hex(0xaaccbb), 0);
    lv_label_set_text(s_bar_bat, LV_SYMBOL_BATTERY_EMPTY " --");

    s_bar_m = lv_label_create(row_stat);
    lv_label_set_text(s_bar_m, "M");
    lv_obj_set_style_text_align(s_bar_m, LV_TEXT_ALIGN_CENTER, 0);

    s_bar_c = lv_label_create(row_stat);
    lv_label_set_text(s_bar_c, "C");

    s_bar_d = lv_label_create(row_stat);
    lv_label_set_text(s_bar_d, "D");

    s_bar_line = lv_label_create(row_stat);
    lv_obj_set_flex_grow(s_bar_line, 1);
    lv_obj_set_style_min_width(s_bar_line, 0, 0);
    lv_label_set_long_mode(s_bar_line, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_bar_line, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_bar_line, LV_SYMBOL_REFRESH " ...");
    lv_obj_set_style_text_color(s_bar_line, lv_color_hex(0xddbb44), 0);

    lv_obj_t *b_info = lv_button_create(top);
    lv_obj_set_size(b_info, 26, 22);
    lv_obj_set_style_pad_all(b_info, 0, 0);
    lv_obj_set_style_radius(b_info, 3, 0);
    lv_obj_set_style_bg_color(b_info, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b_info, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b_info, 1, 0);
    lv_obj_set_style_shadow_width(b_info, 0, 0);
    lv_obj_t *li = lv_label_create(b_info);
    lv_label_set_text(li, "?");
    lv_obj_set_style_text_color(li, lv_color_hex(0x88ffcc), 0);
    lv_obj_center(li);
    lv_obj_add_event_cb(b_info, on_info_btn, LV_EVENT_CLICKED, disp);

    lv_obj_t *stage = lv_obj_create(outer);
    lv_obj_set_width(stage, lv_pct(100));
    lv_obj_set_flex_grow(stage, 1);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_set_style_pad_top(stage, KARAOKE_HEADER_TO_CDG_GAP_PX, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(stage, LV_FLEX_FLOW_COLUMN);
    /* CDG slot first so it sits directly under the header; flex spacer below eats unused height
     * (was a large gap above the slot when the slot was bottom-pinned). */
    lv_obj_set_flex_align(stage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(stage);

    {
        const int fp = KARAOKE_CDG_FRAME_PAD;
        const int fb = KARAOKE_CDG_FRAME_BORDER;
        const int fw = DASHCDG_BADGE_RX_VISIBLE_W + 2 * (fp + fb);
        const int fh = DASHCDG_BADGE_RX_VISIBLE_H + 2 * (fp + fb);
        lv_obj_t *cdg_frame = lv_obj_create(stage);
        lv_obj_set_size(cdg_frame, fw, fh);
        lv_obj_set_style_pad_all(cdg_frame, fp, 0);
        lv_obj_set_style_border_width(cdg_frame, fb, 0);
        lv_obj_set_style_border_color(cdg_frame, lv_color_hex(0x00ccaa), 0);
        lv_obj_set_style_radius(cdg_frame, KARAOKE_CDG_FRAME_RADIUS, 0);
        lv_obj_set_style_clip_corner(cdg_frame, true, 0);
        lv_obj_set_style_bg_opa(cdg_frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(cdg_frame, 0, 0);
        lv_obj_remove_flag(cdg_frame, LV_OBJ_FLAG_CLICKABLE);
        dashcdg_ui_no_scroll(cdg_frame);
        s_cdg_frame = cdg_frame;

        s_cdg_slot = lv_obj_create(cdg_frame);
        lv_obj_set_size(s_cdg_slot, DASHCDG_BADGE_RX_VISIBLE_W, DASHCDG_BADGE_RX_VISIBLE_H);
        lv_obj_center(s_cdg_slot);
        lv_obj_set_style_bg_opa(s_cdg_slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_cdg_slot, 0, 0);
        lv_obj_remove_flag(s_cdg_slot, LV_OBJ_FLAG_CLICKABLE);
        dashcdg_ui_no_scroll(s_cdg_slot);
    }

    s_audio_only_panel = lv_obj_create(stage);
    lv_obj_set_width(s_audio_only_panel, lv_pct(100));
    lv_obj_set_flex_grow(s_audio_only_panel, 1);
    lv_obj_set_style_pad_all(s_audio_only_panel, 10, 0);
    lv_obj_set_style_pad_row(s_audio_only_panel, 14, 0);
    lv_obj_set_style_border_width(s_audio_only_panel, 2, 0);
    lv_obj_set_style_border_color(s_audio_only_panel, lv_color_hex(0x226655), 0);
    lv_obj_set_style_radius(s_audio_only_panel, 12, 0);
    lv_obj_set_style_bg_color(s_audio_only_panel, lv_color_hex(0x060d0a), 0);
    lv_obj_set_style_bg_opa(s_audio_only_panel, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_audio_only_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_audio_only_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_audio_only_panel, LV_OBJ_FLAG_HIDDEN);
    dashcdg_ui_no_scroll(s_audio_only_panel);

    s_audio_track_lbl = lv_label_create(s_audio_only_panel);
    lv_label_set_long_mode(s_audio_track_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_audio_track_lbl, lv_pct(100));
    lv_label_set_text(s_audio_track_lbl, "Track: (waiting)");
    lv_obj_set_style_text_align(s_audio_track_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_audio_track_lbl, lv_color_hex(0xd0e8df), 0);

    {
        lv_obj_t *vol_row = lv_obj_create(s_audio_only_panel);
        lv_obj_set_width(vol_row, lv_pct(100));
        lv_obj_set_height(vol_row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(vol_row, 0, 0);
        lv_obj_set_style_border_width(vol_row, 0, 0);
        lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(vol_row, 8, 0);
        dashcdg_ui_no_scroll(vol_row);

        lv_obj_t *vol_cap = lv_label_create(vol_row);
        lv_label_set_text(vol_cap, "Vol");
        lv_obj_set_style_text_color(vol_cap, lv_color_hex(0x9aabaa), 0);

        s_audio_vol_slider = lv_slider_create(vol_row);
        lv_obj_set_flex_grow(s_audio_vol_slider, 1);
        lv_slider_set_range(s_audio_vol_slider, 0, 100);
        lv_slider_set_value(s_audio_vol_slider, (int32_t)dashcdg_platform_hw_get_karaoke_output_volume_pct(), LV_ANIM_OFF);
        lv_obj_add_event_cb(s_audio_vol_slider, on_audio_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);

        s_audio_vol_pct_lbl = lv_label_create(vol_row);
        lv_label_set_text(s_audio_vol_pct_lbl, "--%");
        lv_obj_set_style_text_color(s_audio_vol_pct_lbl, lv_color_hex(0x88ddbb), 0);
        lv_obj_set_style_min_width(s_audio_vol_pct_lbl, 44, 0);
    }

    s_audio_mute_btn = lv_button_create(s_audio_only_panel);
    lv_obj_set_width(s_audio_mute_btn, lv_pct(92));
    lv_obj_set_height(s_audio_mute_btn, 54);
    lv_obj_set_style_radius(s_audio_mute_btn, 10, 0);
    lv_obj_set_style_bg_color(s_audio_mute_btn, lv_color_hex(0x1a3328), 0);
    lv_obj_set_style_pad_all(s_audio_mute_btn, 10, 0);
    s_audio_mute_lbl = lv_label_create(s_audio_mute_btn);
    lv_label_set_long_mode(s_audio_mute_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_audio_mute_lbl, lv_pct(92));
    lv_obj_set_style_text_align(s_audio_mute_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_audio_mute_lbl, LV_SYMBOL_MUTE "  Mute");
    lv_obj_center(s_audio_mute_lbl);
    lv_obj_add_event_cb(s_audio_mute_btn, on_audio_mute_btn, LV_EVENT_CLICKED, NULL);
    karaoke_audio_refresh_vol_widgets();

    lv_obj_t *stage_fill = lv_obj_create(stage);
    s_stage_fill = stage_fill;
    lv_obj_set_width(stage_fill, lv_pct(100));
    lv_obj_set_flex_grow(stage_fill, 1);
    lv_obj_set_style_min_height(stage_fill, 0, 0);
    lv_obj_set_style_bg_opa(stage_fill, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage_fill, 0, 0);
    dashcdg_ui_no_scroll(stage_fill);
    lv_obj_remove_flag(stage_fill, LV_OBJ_FLAG_CLICKABLE);

    if (!dashcdg_display_lcd_panel()) {
        ESP_LOGE(TAG, "LCD panel handle missing");
        lv_obj_t *err = lv_label_create(outer);
        lv_label_set_long_mode(err, LV_LABEL_LONG_MODE_WRAP);
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
    {
        uint64_t t0 = dashcdg_clock_now_ms();
        dashcdg_badge_rx_stats_t st0;
        dashcdg_badge_rx_get_stats(&st0);
        karaoke_ui_decode_layout_prefs_reload();
        karaoke_ui_apply_stage_from_cached_decode_prefs(&st0);
        karaoke_status_bar_update_slow();
        karaoke_status_bar_update_fast(&st0);
        s_status_slow_deadline_ms = t0 + (uint64_t)KARAOKE_STATUS_SLOW_PERIOD_MS;
    }
    lv_refr_now(disp);
    lvgl_port_unlock();

    s_tick_ctx.disp = disp;
    s_tick = lv_timer_create(on_tick, 33, &s_tick_ctx);
    if (!s_tick) {
        ESP_LOGW(TAG, "lv_timer_create failed");
    }

    dashcdg_platform_hw_notify_activity();
    ESP_LOGI(TAG, "karaoke UI up");
    return ESP_OK;
}
