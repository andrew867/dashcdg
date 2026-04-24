/*
 * v4 UDP multicast receiver task: parse wire packets, CDG jitter drain, raster to panel (band blit).
 */
#include "badge_rx.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_rom_sys.h"
#include "lvgl.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "dashcdg/cdg.h"
#include "dashcdg/cdg_batch_jitter.h"
#include "dashcdg/fec.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/protocol.h"

#include "badge_cdg_rgb565.h"
#include "display_lvgl.h"

static const char *TAG = "badge_rx";

#define BADGE_RX_MCAST_ADDR "239.255.77.77"
#define BADGE_RX_PORT       24684
#define BADGE_RX_STACK      8192
#define BADGE_RX_TASK_PRIO  5
#define BADGE_RX_RCVBUF     (32 * 1024)
#define BADGE_CDG_LATE_GRACE_MS 120U
#define BADGE_RX_STATS_INTERVAL_MS 2000U
/*
 * Video-only badge receiver does not provide real audio playout telemetry
 * (presented_audio_timestamp_ms / host latency), so v4_rx_stats can mislead TX controller health.
 */
#define BADGE_RX_ENABLE_TX_V4_STATS 0
/** Keep this many jitter slots free; evict furthest-ahead batches when tighter. */
#define BADGE_RX_JB_HEADROOM 6U
#define BADGE_RX_CDG_REPAIR_GROUP_SIZE 9U
#define BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX 144U
/*
 * Keep full-height blits for correctness. Partial-height pressure clipping produced visible banding/
 * stale lower scanlines (wrong palette stripes + bottom held color) under current overlay path.
 */
#define BADGE_RX_CDG_BLIT_PARTIAL_H DASHCDG_BADGE_RX_VISIBLE_H
/* SPI write settle gap per band: avoids reusing one scratch band while prior DMA transfer is still active. */
#define BADGE_RX_PANEL_BAND_SETTLE_US 2200U
#define BADGE_RX_TRACKED_VIDEO_REPAIR_GROUPS 2U

/** RGB565 band scratch (~6.9 KiB): keep off .bss so CDG+jitter static fits in dram0_0_seg. */
#define BADGE_RX_BLIT_SCRATCH_BYTES \
    ((size_t)DASHCDG_BADGE_RX_VISIBLE_W * (size_t)DASHCDG_BADGE_RX_BLIT_BAND_H * sizeof(uint16_t))

/** v4 RLE canvas anchor (same layout as desktop `app_rx.c`). */
#define BADGE_RX_CDG_SNAPSHOT_STATE_BYTES \
    (2U + DASHCDG_COLORS + ((size_t)DASHCDG_COLORS * 4U) + ((size_t)DASHCDG_SCREEN_WIDTH * (size_t)DASHCDG_SCREEN_HEIGHT))
#define BADGE_RX_V4_ANCHOR_RX_CHUNK_STRIDE 512U
#define BADGE_RX_V4_ANCHOR_ENCODED_MAX_BYTES (4U + (BADGE_RX_CDG_SNAPSHOT_STATE_BYTES * 2U))
#define BADGE_RX_V4_ANCHOR_CHUNK_COUNT \
    ((BADGE_RX_V4_ANCHOR_ENCODED_MAX_BYTES + BADGE_RX_V4_ANCHOR_RX_CHUNK_STRIDE - 1U) / BADGE_RX_V4_ANCHOR_RX_CHUNK_STRIDE)

struct badge_rx_video_repair_group {
    uint8_t occupied;
    uint8_t expected_group_size;
    uint8_t parity_present;
    uint8_t member_present[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint16_t member_lengths[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint8_t member_payloads[BADGE_RX_CDG_REPAIR_GROUP_SIZE][BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX];
    uint32_t group_id;
    struct dashcdg_fec_parity_state parity;
};

static TaskHandle_t s_rx_task;
static volatile int s_sock = -1;
static volatile int s_run;

static SemaphoreHandle_t s_mtx;

/*
 * CDG + jitter in static .dram0.bss; band blit scratch on internal heap (~7 KiB) to stay under dram0_0_seg.
 * v4 RLE anchors expand straight into `s_cdg` (no separate ~64 KiB decode buffer).
 * dashcdg_badge_rx_stop() does not tear down CDG/jitter/scratch so karaoke re-entry stays stable.
 */
static struct dashcdg_cdg_state s_cdg_storage;
static struct dashcdg_cdg_batch_jitter_buffer s_jb_storage;
static struct dashcdg_cdg_state *s_cdg;
static struct dashcdg_cdg_batch_jitter_buffer *s_jb;
static struct dashcdg_media_clock s_mclk;

static uint16_t *s_cdg_blit_scratch;

static uint64_t s_sync_local_ms;
static uint64_t s_sync_playback_ms;
static uint16_t s_announced_playout_delay_ms;

static dashcdg_badge_rx_stats_t s_stats;
static int s_jitter_cdg_primed;
static uint64_t s_last_cdg_apply_local_ms;

/** Network byte order; source IP of last unicast v4 datagram (TX host). */
static uint32_t s_v4_tx_src_ipv4;
static uint64_t s_last_v4_stats_sent_ms;
static uint32_t s_rx_stats_seq;
static uint32_t s_badge_receiver_instance_id;
static int s_badge_receiver_instance_inited;
static uint64_t s_active_session_start_ms;
static uint32_t s_active_asset_size;
static char s_active_song_id[DASHCDG_MAX_SONG_ID];
static int s_active_session_valid;
static struct badge_rx_video_repair_group s_video_repair_groups[BADGE_RX_TRACKED_VIDEO_REPAIR_GROUPS];

/** Clip CDG panel blit to Y in [0, max); full frame when not under jitter pressure. */
static uint16_t s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;

/** v4 VIDEO_ANCHOR chunk assembly (heap; wire total_bytes is RLE-compressed, bounded by protocol). */
static uint8_t *s_v4_anchor_asm_buf;
static uint32_t s_v4_anchor_asm_id;
static uint64_t s_v4_anchor_asm_packet_index;
static uint32_t s_v4_anchor_asm_total_bytes;
static size_t s_v4_anchor_asm_received_bytes;
static uint8_t s_v4_anchor_chunk_seen[BADGE_RX_V4_ANCHOR_CHUNK_COUNT];
static uint32_t s_cdg_snapshots_applied;

static void drain_cdg_to_idle(uint64_t local_now_ms);

static int badge_rx_ipv4_is_unicast_src(uint32_t addr_be)
{
    uint32_t h = ntohl(addr_be);

    if (h == 0U || h == 0xffffffffU) {
        return 0;
    }
    if ((h >> 28) == 0xEU) {
        return 0;
    }
    return 1;
}

static uint32_t badge_rx_read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24U) | ((uint32_t)src[1] << 16U) | ((uint32_t)src[2] << 8U) | (uint32_t)src[3];
}

static void badge_rx_v4_anchor_asm_reset(void)
{
    if (s_v4_anchor_asm_buf != NULL) {
        heap_caps_free(s_v4_anchor_asm_buf);
        s_v4_anchor_asm_buf = NULL;
    }
    s_v4_anchor_asm_id = 0U;
    s_v4_anchor_asm_packet_index = 0U;
    s_v4_anchor_asm_total_bytes = 0U;
    s_v4_anchor_asm_received_bytes = 0U;
    memset(s_v4_anchor_chunk_seen, 0, sizeof(s_v4_anchor_chunk_seen));
}

static void badge_rx_v4_anchor_begin(uint32_t anchor_id, uint64_t packet_index, uint32_t total_bytes)
{
    badge_rx_v4_anchor_asm_reset();
    if (total_bytes == 0U || total_bytes > BADGE_RX_V4_ANCHOR_ENCODED_MAX_BYTES) {
        return;
    }
    s_v4_anchor_asm_buf = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_v4_anchor_asm_buf == NULL) {
        ESP_LOGW(TAG, "v4 anchor asm malloc %u failed", (unsigned)total_bytes);
        return;
    }
    memset(s_v4_anchor_asm_buf, 0, total_bytes);
    s_v4_anchor_asm_id = anchor_id;
    s_v4_anchor_asm_packet_index = packet_index;
    s_v4_anchor_asm_total_bytes = total_bytes;
}

/** Same RLE walk as decode; no writes — avoids corrupting `s_cdg` on malformed anchors. */
static int badge_rx_validate_v4_anchor_rle(const uint8_t *enc, size_t enc_len)
{
    size_t src = 4U;
    size_t dst_o = 0U;
    uint32_t expected;

    if (enc_len < 4U) {
        return 0;
    }
    expected = badge_rx_read_u32_be(enc);
    if (expected != (uint32_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    while (src + 1U < enc_len && dst_o < (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES) {
        uint8_t run = enc[src++];
        uint8_t v = enc[src++];
        (void)v;
        if (run == 0U) {
            return 0;
        }
        for (uint8_t k = 0; k < run && dst_o < (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES; k++) {
            dst_o++;
        }
    }
    return dst_o == (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES;
}

/**
 * RLE after 4-byte BE uncompressed size; snapshot layout matches desktop `dashcdg_rx_load_active_snapshot_state_locked`.
 * Sets `st->ts` from `packet_index` on success only.
 */
static int badge_rx_decode_v4_anchor_rle_into_cdg(struct dashcdg_cdg_state *st, uint64_t packet_index, const uint8_t *enc,
                                                  size_t enc_len)
{
    size_t src = 4U;
    size_t dst_o = 0U;
    uint32_t expected;
    uint8_t ct_accum[4];

    if (st == NULL || enc_len < 4U) {
        return 0;
    }
    expected = badge_rx_read_u32_be(enc);
    if (expected != (uint32_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    while (src + 1U < enc_len && dst_o < (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES) {
        uint8_t run = enc[src++];
        uint8_t v = enc[src++];
        if (run == 0U) {
            return 0;
        }
        for (uint8_t k = 0; k < run && dst_o < (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES; k++) {
            if (dst_o < 2U) {
                if (dst_o == 0U) {
                    st->display_h_offset = v;
                } else {
                    st->display_v_offset = v;
                }
            } else if (dst_o < 2U + DASHCDG_COLORS) {
                st->transparency[dst_o - 2U] = v;
            } else if (dst_o < 2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U)) {
                size_t rel = dst_o - (2U + DASHCDG_COLORS);
                size_t ci = rel / 4U;
                size_t bi = rel % 4U;

                ct_accum[bi] = v;
                if (bi == 3U) {
                    st->color_table[ci] = (int)badge_rx_read_u32_be(ct_accum);
                }
            } else {
                size_t fi = dst_o - (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U));

                st->framebuffer[fi] = v;
            }
            dst_o++;
        }
    }
    if (dst_o != (size_t)BADGE_RX_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    st->ts = (dashcdg_tick_t)packet_index;
    return 1;
}

static void badge_rx_fill_visible_rect(struct dashcdg_cdg_state *st, int x, int y, int w, int h, uint8_t color_idx)
{
    int x0 = x < DASHCDG_VISIBLE_X ? DASHCDG_VISIBLE_X : x;
    int y0 = y < DASHCDG_VISIBLE_Y ? DASHCDG_VISIBLE_Y : y;
    int x1 = x + w;
    int y1 = y + h;
    uint8_t ci = (uint8_t)(color_idx & 0x0FU);

    if (st == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (x1 > DASHCDG_VISIBLE_RIGHT) {
        x1 = DASHCDG_VISIBLE_RIGHT;
    }
    if (y1 > DASHCDG_VISIBLE_BOTTOM) {
        y1 = DASHCDG_VISIBLE_BOTTOM;
    }
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            st->framebuffer[DASHCDG_ARRAY_INDEX(col, row)] = ci;
        }
    }
}

/** Same visual language as desktop `dashcdg_rx_render_connecting_state` (stripes + pulse), no font. */
static void badge_rx_paint_connecting_pattern(struct dashcdg_cdg_state *st, uint64_t now_ms, int reconnecting)
{
    int pulse = (int)((now_ms / 250U) % 4U);
    int stripe_phase = (int)((now_ms / 400U) % 6U);

    if (st == NULL) {
        return;
    }
    dashcdg_cdg_state_init(st);
    st->color_table[0] = 0x05070F;
    st->color_table[1] = 0x101A34;
    st->color_table[2] = 0x204070;
    st->color_table[3] = 0x4F8BFF;
    st->color_table[4] = 0x63E6BE;
    st->color_table[5] = 0xFFE066;
    st->color_table[6] = 0xFF8FA3;
    st->color_table[7] = 0xFFFFFF;
    memset(st->transparency, 0, sizeof(st->transparency));

    badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X, DASHCDG_VISIBLE_Y, DASHCDG_VISIBLE_WIDTH, DASHCDG_VISIBLE_HEIGHT, 1);

    for (int stripe = 0; stripe < 6; ++stripe) {
        uint8_t c = (uint8_t)(((stripe + stripe_phase) % 3) == 0 ? 2 : (((stripe + stripe_phase) % 3) == 1 ? 3 : 4));

        badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X, DASHCDG_VISIBLE_Y + (stripe * 12), DASHCDG_VISIBLE_WIDTH, 5, c);
        badge_rx_fill_visible_rect(
                st, DASHCDG_VISIBLE_X, DASHCDG_VISIBLE_BOTTOM - ((stripe + 1) * 12), DASHCDG_VISIBLE_WIDTH, 5, c);
    }

    badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X + 34, DASHCDG_VISIBLE_Y + 132, 212, 12, 2);
    badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X + 40 + (pulse * 48), DASHCDG_VISIBLE_Y + 128, 28, 20,
                               reconnecting ? 6 : 5);
    badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X + 54, DASHCDG_VISIBLE_Y + 166, 184, 8, 3);
    badge_rx_fill_visible_rect(st, DASHCDG_VISIBLE_X + 54 + (pulse * 44), DASHCDG_VISIBLE_Y + 160, 22, 20, 7);
    dashcdg_cdg_state_raster_dirty_mark_full(st);
}

static void badge_rx_handle_v4_loading_screen(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    int reconnecting;
    const struct dashcdg_v4_loading_screen_payload *ls;

    if (view == NULL || s_cdg == NULL || s_jb == NULL) {
        return;
    }
    if (s_cdg_snapshots_applied > 0U || s_jb->initialized || s_jitter_cdg_primed) {
        return;
    }

    ls = &view->v4_loading_screen;
    reconnecting = (ls->screen_kind == DASHCDG_V4_LOADING_SCREEN_REPAIRING ||
                      ls->screen_kind == DASHCDG_V4_LOADING_SCREEN_LATE_JOIN)
                             ? 1
                             : 0;
    badge_rx_paint_connecting_pattern(s_cdg, local_now_ms + ((uint64_t)ls->animation_phase * 125U), reconnecting);
    s_stats.v4_loading_screen_count++;
}

static int badge_rx_should_apply_v4_anchor(uint64_t anchor_packet_index)
{
    if (s_cdg_snapshots_applied == 0U) {
        if (s_jb != NULL && s_jb->initialized && anchor_packet_index < s_jb->next_packet_index) {
            return 0;
        }
        return 1;
    }
    if (s_jb == NULL || !s_jb->initialized) {
        return 0;
    }
    if (anchor_packet_index < s_jb->next_packet_index) {
        return 0;
    }
    return 1;
}

/** Returns 1 if snapshot was applied to CDG + jitter seek. */
static int badge_rx_try_apply_assembled_v4_anchor(void)
{
    if (s_v4_anchor_asm_buf == NULL || s_v4_anchor_asm_received_bytes < (size_t)s_v4_anchor_asm_total_bytes ||
        s_cdg == NULL || s_jb == NULL) {
        return 0;
    }

    if (!badge_rx_should_apply_v4_anchor(s_v4_anchor_asm_packet_index)) {
        badge_rx_v4_anchor_asm_reset();
        return 0;
    }
    if (!badge_rx_validate_v4_anchor_rle(s_v4_anchor_asm_buf, s_v4_anchor_asm_total_bytes)) {
        ESP_LOGW(TAG, "v4 anchor RLE invalid (id=%u total=%u)", (unsigned)s_v4_anchor_asm_id,
                 (unsigned)s_v4_anchor_asm_total_bytes);
        badge_rx_v4_anchor_asm_reset();
        return 0;
    }
    if (!badge_rx_decode_v4_anchor_rle_into_cdg(s_cdg, s_v4_anchor_asm_packet_index, s_v4_anchor_asm_buf,
                                                s_v4_anchor_asm_total_bytes)) {
        ESP_LOGW(TAG, "v4 anchor RLE decode into CDG failed (id=%u)", (unsigned)s_v4_anchor_asm_id);
        badge_rx_v4_anchor_asm_reset();
        return 0;
    }

    {
        uint32_t log_id = s_v4_anchor_asm_id;
        uint64_t log_pkt = s_v4_anchor_asm_packet_index;

        dashcdg_cdg_batch_jitter_apply_snapshot_seek(s_jb, s_v4_anchor_asm_packet_index);
        s_jitter_cdg_primed = 1;
        s_last_cdg_apply_local_ms = dashcdg_clock_now_ms();
        s_cdg_snapshots_applied++;
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
        dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
        badge_rx_v4_anchor_asm_reset();
        ESP_LOGI(TAG, "v4 anchor applied id=%u pkt=%llu", (unsigned)log_id, (unsigned long long)log_pkt);
    }
    return 1;
}

static void badge_rx_handle_v4_video_anchor(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    const struct dashcdg_v4_video_anchor_payload *va;
    size_t chunk_index;

    if (view == NULL) {
        return;
    }
    va = &view->v4_video_anchor;
    if (va->anchor_bytes == NULL || va->total_bytes == 0U ||
        va->total_bytes > BADGE_RX_V4_ANCHOR_ENCODED_MAX_BYTES || va->chunk_length == 0U ||
        va->anchor_offset + va->chunk_length > va->total_bytes) {
        return;
    }
    if (va->anchor_format != DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS) {
        return;
    }
    if (s_cdg == NULL || s_jb == NULL) {
        return;
    }

    if (s_v4_anchor_asm_id != va->anchor_id || s_v4_anchor_asm_packet_index != va->packet_index ||
        s_v4_anchor_asm_total_bytes != va->total_bytes) {
        badge_rx_v4_anchor_begin(va->anchor_id, va->packet_index, va->total_bytes);
    }
    if (s_v4_anchor_asm_buf == NULL) {
        return;
    }

    chunk_index = (size_t)va->anchor_offset / BADGE_RX_V4_ANCHOR_RX_CHUNK_STRIDE;
    if (chunk_index >= BADGE_RX_V4_ANCHOR_CHUNK_COUNT || s_v4_anchor_chunk_seen[chunk_index]) {
        return;
    }

    memcpy(s_v4_anchor_asm_buf + va->anchor_offset, va->anchor_bytes, va->chunk_length);
    s_v4_anchor_chunk_seen[chunk_index] = 1U;
    s_v4_anchor_asm_received_bytes += va->chunk_length;

    if (s_v4_anchor_asm_received_bytes >= (size_t)s_v4_anchor_asm_total_bytes) {
        if (badge_rx_try_apply_assembled_v4_anchor()) {
            drain_cdg_to_idle(local_now_ms);
        }
    }
}

static void badge_rx_ensure_receiver_instance_id(void)
{
    uint8_t mac[6];

    if (s_badge_receiver_instance_inited) {
        return;
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        (void)esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    }
    s_badge_receiver_instance_id =
        ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    if (s_badge_receiver_instance_id == 0U) {
        s_badge_receiver_instance_id = 0xBAD6E000U;
    }
    s_badge_receiver_instance_inited = 1;
}

static void badge_rx_maybe_send_v4_stats(int sockfd, uint64_t now_ms)
{
    if (!BADGE_RX_ENABLE_TX_V4_STATS) {
        return;
    }
    uint8_t pkt[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_rx_stats_payload pl;
    struct sockaddr_in dst;
    size_t sz;
    uint32_t tx_ip;
    ssize_t sent;

    if (sockfd < 0) {
        return;
    }
    if (s_last_v4_stats_sent_ms != 0U && (now_ms - s_last_v4_stats_sent_ms) < (uint64_t)BADGE_RX_STATS_INTERVAL_MS) {
        return;
    }

    badge_rx_ensure_receiver_instance_id();
    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(40)) != pdTRUE) {
        return;
    }
    tx_ip = s_v4_tx_src_ipv4;
    if (tx_ip == 0U) {
        xSemaphoreGive(s_mtx);
        return;
    }

    {
        uint16_t preroll_ms = s_announced_playout_delay_ms;
        uint32_t jb_pending = 0U;
        int have_clock = s_stats.have_clock;
        int skew_inited = s_stats.skew_ema_inited;
        int32_t skew_ms = s_stats.skew_ema_ms;
        uint32_t sess_ct = s_stats.v4_session_count;
        uint16_t startup_stage = DASHCDG_V4_RX_STARTUP_WAIT_ANNOUNCE;
        uint32_t startup_flags = 0U;
        uint64_t sender_obs_ms = 0U;

        if (preroll_ms == 0U) {
            preroll_ms = 500U;
        }
        if (s_jb != NULL) {
            jb_pending = (uint32_t)dashcdg_cdg_batch_jitter_occupied_count(s_jb);
        }
        if (sess_ct > 0U) {
            startup_stage = DASHCDG_V4_RX_STARTUP_V4_METADATA;
        }
        if (have_clock) {
            startup_stage = DASHCDG_V4_RX_STARTUP_RUNNING;
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_HAVE_CLOCK;
            sender_obs_ms = (uint64_t)dashcdg_media_clock_remote_now(&s_mclk, (int64_t)now_ms);
        }

        hdr.sequence = ++s_rx_stats_seq;
        hdr.sender_time_ms = now_ms;

        pl.report_seq = hdr.sequence;
        pl.wall_now_ms = now_ms;
        pl.sender_time_observed_ms = sender_obs_ms;
        pl.clock_offset_estimate_ms = skew_inited ? -skew_ms : 0;
        pl.playout_delay_ms_config = preroll_ms;
        pl.reserved0 = 0;
        pl.audio_buffer_ms = jb_pending * 48U;
        pl.audio_queue_pressure_events = 0U;
        pl.fec_audio_recovered = 0U;
        {
            int32_t aj = skew_ms >= 0 ? skew_ms : -skew_ms;
            pl.jitter_rms_ms = (uint16_t)(aj > 65535 ? 65535 : aj);
        }
        pl.loss_pct_x100 = 0U;
        pl.v4_codec_id = 0U;
        pl.opus_bitrate_bps = 0U;
        pl.fec_decode_attempts = 0U;
        pl.fec_recovery_failed = 0U;
        pl.media_datagrams_lost_estimated = 0U;
        pl.cdg_fec_recovered = 0U;
        pl.cdg_fec_failed = 0U;
        pl.jitter_p95_ms = pl.jitter_rms_ms;
        pl.jitter_max_ms = pl.jitter_rms_ms;
        pl.reorder_events = 0U;
        pl.receiver_instance_id = s_badge_receiver_instance_id;
        pl.fec_group_size_observed = 0U;
        pl.presented_audio_timestamp_ms = 0U;
        pl.audio_buffer_target_ms = preroll_ms;
        pl.host_output_latency_ms = 0U;
        pl.target_total_latency_ms = preroll_ms;
        pl.startup_stage = startup_stage;
        pl.drift_trim_ppm = 0;
        pl.recovery_host_underrun_count = 0U;
        pl.recovery_zero_buffer_count = 0U;
        pl.recovery_silent_stall_count = 0U;
        pl.source_idle_park_count = 0U;
        pl.startup_flags = startup_flags;
    }

    sz = dashcdg_protocol_serialize_v4_rx_stats(pkt, sizeof(pkt), &hdr, &pl);
    xSemaphoreGive(s_mtx);

    if (sz == 0U) {
        return;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BADGE_RX_PORT);
    dst.sin_addr.s_addr = tx_ip;

    sent = sendto(sockfd, pkt, sz, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (sent == (ssize_t)sz) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_stats.v4_rx_stats_sent++;
            xSemaphoreGive(s_mtx);
        }
        s_last_v4_stats_sent_ms = now_ms;
    } else if (sent < 0) {
        ESP_LOGD(TAG, "v4 rx-stats sendto errno=%d", errno);
    }
}

/** 0 = ok, -1 = OOM */
static int badge_rx_ensure_blit_scratch(void)
{
    void *p;

    if (s_cdg_blit_scratch != NULL) {
        return 0;
    }
    p = heap_caps_calloc(1, BADGE_RX_BLIT_SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (p == NULL) {
        p = heap_caps_calloc(1, BADGE_RX_BLIT_SCRATCH_BYTES, MALLOC_CAP_INTERNAL);
    }
    if (p == NULL) {
        return -1;
    }
    s_cdg_blit_scratch = (uint16_t *)p;
    return 0;
}

static void badge_rx_free_blit_scratch(void)
{
    if (s_cdg_blit_scratch != NULL) {
        heap_caps_free(s_cdg_blit_scratch);
        s_cdg_blit_scratch = NULL;
    }
}

/** Drop CDG/jitter pointers after a failed start (storage stays in .bss). Frees heap blit scratch. */
static void badge_rx_free_heap(void)
{
    badge_rx_v4_anchor_asm_reset();
    badge_rx_free_blit_scratch();
    if (s_jb != NULL) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
    }
    s_jb = NULL;
    s_cdg = NULL;
}

/** Wire static CDG + jitter; always succeeds if linked. */
static int badge_rx_ensure_heap(void)
{
    if (s_cdg != NULL) {
        return 0;
    }
    s_cdg = &s_cdg_storage;
    s_jb = &s_jb_storage;
    return 0;
}

/** After jitter drains or anchors, restore full 192px blit when queue is not under headroom pressure. */
static void badge_rx_cdg_blit_relax_pressure_clip(void)
{
    if (s_jb == NULL || !s_jb->initialized) {
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
        return;
    }
    if (dashcdg_cdg_batch_jitter_occupied_count(s_jb) + BADGE_RX_JB_HEADROOM <= DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT) {
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    }
}

static void apply_cdg_batch(const struct dashcdg_cdg_batch_jitter_frame *batch)
{
    if (batch == NULL || s_cdg == NULL || s_jb == NULL) {
        return;
    }
    for (uint8_t i = 0; i < batch->packet_count; ++i) {
        const uint8_t *p = batch->packet_bytes + (size_t)i * DASHCDG_SUBCHANNEL_PACKET_BYTES;
        struct dashcdg_subchannel_packet pkt;
        memcpy(&pkt, p, sizeof(pkt));
        (void)dashcdg_cdg_state_process_packet(s_cdg, &pkt);
    }
    dashcdg_cdg_batch_jitter_note_applied(s_jb, (struct dashcdg_cdg_batch_jitter_frame *)batch);
}

static void drain_cdg_to_idle(uint64_t local_now_ms)
{
    struct dashcdg_cdg_batch_jitter_drain_input din;
    struct dashcdg_cdg_batch_jitter_frame *batch = NULL;
    uint64_t miss = 0;
    int guard = 0;

    if (s_jb == NULL || !s_jb->initialized) {
        return;
    }

    for (;;) {
        if (++guard > 48) {
            break;
        }
        memset(&din, 0, sizeof(din));
        if (s_stats.have_clock) {
            int64_t rnow = dashcdg_media_clock_remote_now(&s_mclk, (int64_t)local_now_ms);
            int64_t rthen = dashcdg_media_clock_remote_now(&s_mclk, (int64_t)s_sync_local_ms);
            din.have_sender_playback = 1;
            din.sender_playback_now_ms = s_sync_playback_ms + (uint64_t)(rnow - rthen);
            din.announced_playout_delay_ms = s_announced_playout_delay_ms;
        }
        din.late_grace_ms = BADGE_CDG_LATE_GRACE_MS;
        din.late_gate = (s_jitter_cdg_primed || dashcdg_cdg_batch_jitter_occupied_count(s_jb) > 0U) ? 1 : 0;
        din.ms_since_prior_cdg_apply = 0U;
        if (s_last_cdg_apply_local_ms != 0U) {
            din.ms_since_prior_cdg_apply =
                (local_now_ms > s_last_cdg_apply_local_ms) ? (local_now_ms - s_last_cdg_apply_local_ms) : 0U;
        }
        din.primed_decode = s_jitter_cdg_primed;

        enum dashcdg_cdg_batch_drain_step step =
            dashcdg_cdg_batch_jitter_drain_step(s_jb, &din, &batch, &miss);
        if (step == DASHCDG_CDG_BATCH_DRAIN_SKIP) {
            s_stats.live_missing_skips += miss;
            s_last_cdg_apply_local_ms = local_now_ms;
        } else if (step == DASHCDG_CDG_BATCH_DRAIN_APPLY && batch != NULL) {
            apply_cdg_batch(batch);
            s_jitter_cdg_primed = 1;
            s_last_cdg_apply_local_ms = local_now_ms;
        } else {
            break;
        }
    }
    badge_rx_cdg_blit_relax_pressure_clip();
}

/** If CDG/jitter pointers were dropped (e.g. failed rx_start), wire static storage again. */
static void badge_rx_try_alloc_cdg_jitter(void)
{
    if (s_cdg != NULL && s_jb != NULL) {
        return;
    }
    if (badge_rx_ensure_heap() != 0) {
        return;
    }
    dashcdg_cdg_state_init(s_cdg);
    dashcdg_cdg_batch_jitter_init(s_jb);
    s_jitter_cdg_primed = 0;
    s_last_cdg_apply_local_ms = 0U;
    dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
    s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    (void)badge_rx_ensure_blit_scratch();
    ESP_LOGI(TAG, "CDG/jitter buffers allocated (late bind)");
}

void dashcdg_badge_rx_try_upgrade_cdg_heap(void)
{
    if (s_rx_task == NULL) {
        return;
    }
    if (s_cdg != NULL && s_jb != NULL) {
        return;
    }
    ESP_LOGD(TAG, "try CDG static bind: internal free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    badge_rx_try_alloc_cdg_jitter();
}

static void handle_session_info(const struct dashcdg_packet_view *view)
{
    int same_session;

    badge_rx_try_alloc_cdg_jitter();
    s_announced_playout_delay_ms = view->v4_session_info.startup_preroll_ms;

    same_session = (s_active_session_valid &&
                    s_active_session_start_ms == view->v4_session_info.session_start_ms &&
                    memcmp(s_active_song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID) == 0 &&
                    s_active_asset_size == view->v4_session_info.asset_size);
    if (same_session) {
        return;
    }
    s_active_session_start_ms = view->v4_session_info.session_start_ms;
    s_active_asset_size = view->v4_session_info.asset_size;
    memcpy(s_active_song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID);
    s_active_session_valid = 1;

    s_cdg_snapshots_applied = 0U;
    badge_rx_v4_anchor_asm_reset();
    memset(s_video_repair_groups, 0, sizeof(s_video_repair_groups));

    s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;

    if (s_cdg != NULL) {
        dashcdg_cdg_state_init(s_cdg);
    }
    if (s_jb != NULL) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
    }
    s_jitter_cdg_primed = 0;
    s_last_cdg_apply_local_ms = 0U;
    s_stats.skew_ema_inited = 0;
    memset(s_stats.song_id, 0, sizeof(s_stats.song_id));
    memcpy(s_stats.song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID);
    s_stats.song_id[DASHCDG_MAX_SONG_ID] = '\0';
    s_stats.v4_session_count++;
    s_stats.have_clock = 0;
    dashcdg_media_clock_init(&s_mclk);
    if (s_cdg != NULL) {
        dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
    }
}

static void handle_clock_sync(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    dashcdg_media_clock_anchor(&s_mclk, (int64_t)local_now_ms, (int64_t)view->header.sender_time_ms);
    s_sync_local_ms = local_now_ms;
    s_sync_playback_ms = view->v4_clock_sync.playback_ms;
    s_stats.have_clock = 1;
    s_stats.v4_clock_count++;
}

static void badge_rx_video_repair_clear_group(struct badge_rx_video_repair_group *g)
{
    if (g == NULL) {
        return;
    }
    memset(g, 0, sizeof(*g));
}

static struct badge_rx_video_repair_group *badge_rx_video_repair_get_group(uint32_t group_id)
{
    struct badge_rx_video_repair_group *free_group = NULL;
    struct badge_rx_video_repair_group *oldest_group = NULL;

    for (size_t i = 0; i < BADGE_RX_TRACKED_VIDEO_REPAIR_GROUPS; ++i) {
        struct badge_rx_video_repair_group *g = &s_video_repair_groups[i];

        if (g->occupied) {
            if (g->group_id == group_id) {
                return g;
            }
            if (oldest_group == NULL || g->group_id < oldest_group->group_id) {
                oldest_group = g;
            }
        } else if (free_group == NULL) {
            free_group = g;
        }
    }
    if (free_group != NULL) {
        badge_rx_video_repair_clear_group(free_group);
        free_group->occupied = 1U;
        free_group->group_id = group_id;
        return free_group;
    }
    if (oldest_group != NULL) {
        badge_rx_video_repair_clear_group(oldest_group);
        oldest_group->occupied = 1U;
        oldest_group->group_id = group_id;
        return oldest_group;
    }
    return NULL;
}

static void badge_rx_video_repair_try_recover_group(struct badge_rx_video_repair_group *g)
{
    const uint8_t *known_payloads[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint16_t known_lengths[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint8_t recovered_payload[BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX];
    uint16_t recovered_length = 0U;
    size_t known_count = 0U;
    int missing_index = -1;

    if (g == NULL || !g->occupied || !g->parity_present || g->expected_group_size <= 1U) {
        return;
    }
    for (uint8_t i = 0U; i < g->expected_group_size; ++i) {
        if (g->member_present[i]) {
            known_payloads[known_count] = g->member_payloads[i];
            known_lengths[known_count] = g->member_lengths[i];
            known_count++;
        } else if (missing_index < 0) {
            missing_index = (int)i;
        } else {
            return;
        }
    }
    if (missing_index < 0 || known_count + 1U != g->expected_group_size) {
        return;
    }
    if (!dashcdg_fec_parity_recover(&g->parity, known_payloads, known_lengths, known_count, recovered_payload,
                                    &recovered_length)) {
        s_stats.v4_video_repair_failed++;
        g->parity_present = 0U;
        return;
    }
    if (recovered_length == 0U || recovered_length > BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX ||
        (recovered_length % DASHCDG_SUBCHANNEL_PACKET_BYTES) != 0U) {
        s_stats.v4_video_repair_failed++;
        g->parity_present = 0U;
        return;
    }
    {
        uint8_t packet_count = (uint8_t)(recovered_length / DASHCDG_SUBCHANNEL_PACKET_BYTES);
        uint64_t batch_index = (uint64_t)g->group_id * (uint64_t)BADGE_RX_CDG_REPAIR_GROUP_SIZE + (uint64_t)missing_index;
        uint64_t packet_start_index = batch_index * (uint64_t)DASHCDG_MAX_CDG_BATCH_PACKETS;

        if (packet_count == 0U || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
            s_stats.v4_video_repair_failed++;
            g->parity_present = 0U;
            return;
        }
        if (s_jb == NULL ||
            !dashcdg_cdg_batch_jitter_insert(s_jb, packet_start_index, packet_count, recovered_payload, 0)) {
            s_stats.v4_video_repair_failed++;
            return;
        }
        g->member_present[missing_index] = 1U;
        g->member_lengths[missing_index] = recovered_length;
        memcpy(g->member_payloads[missing_index], recovered_payload, recovered_length);
        s_stats.v4_video_repair_recovered++;
    }
}

static void badge_rx_video_repair_observe_delta(uint32_t group_id, uint8_t group_index, const uint8_t *delta_bytes,
                                                uint16_t encoded_length)
{
    struct badge_rx_video_repair_group *g;

    if (delta_bytes == NULL || encoded_length == 0U || encoded_length > BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX ||
        group_index >= BADGE_RX_CDG_REPAIR_GROUP_SIZE) {
        return;
    }
    g = badge_rx_video_repair_get_group(group_id);
    if (g == NULL) {
        return;
    }
    if (g->expected_group_size == 0U || g->expected_group_size > BADGE_RX_CDG_REPAIR_GROUP_SIZE) {
        g->expected_group_size = BADGE_RX_CDG_REPAIR_GROUP_SIZE;
    }
    if (!g->member_present[group_index]) {
        g->member_present[group_index] = 1U;
        g->member_lengths[group_index] = encoded_length;
        memcpy(g->member_payloads[group_index], delta_bytes, encoded_length);
    }
    badge_rx_video_repair_try_recover_group(g);
}

static void handle_video_delta(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    if (view->v4_video_delta.delta_format != DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS) {
        return;
    }
    if (s_jb == NULL) {
        return;
    }
    {
        size_t occ = dashcdg_cdg_batch_jitter_occupied_count(s_jb);

        if (occ + BADGE_RX_JB_HEADROOM > DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT) {
            dashcdg_cdg_batch_jitter_evict_pressure(s_jb, BADGE_RX_JB_HEADROOM);
            s_stats.jb_evict_rounds++;
            if (s_cdg_blit_max_y > BADGE_RX_CDG_BLIT_PARTIAL_H) {
                s_cdg_blit_max_y = (uint16_t)BADGE_RX_CDG_BLIT_PARTIAL_H;
            }
        }
    }
    if (dashcdg_cdg_batch_jitter_insert(s_jb, view->v4_video_delta.packet_start_index, view->v4_video_delta.packet_count,
                                        view->v4_video_delta.delta_bytes, 1)) {
        badge_rx_video_repair_observe_delta(view->v4_video_delta.group_id, view->v4_video_delta.group_index,
                                            view->v4_video_delta.delta_bytes, view->v4_video_delta.encoded_length);
        s_stats.v4_video_delta_count++;
        drain_cdg_to_idle(local_now_ms);
        return;
    }
    dashcdg_cdg_batch_jitter_evict_pressure(s_jb, 2U);
    s_stats.jb_evict_rounds++;
    if (s_cdg_blit_max_y > BADGE_RX_CDG_BLIT_PARTIAL_H) {
        s_cdg_blit_max_y = (uint16_t)BADGE_RX_CDG_BLIT_PARTIAL_H;
    }
    if (dashcdg_cdg_batch_jitter_insert(s_jb, view->v4_video_delta.packet_start_index, view->v4_video_delta.packet_count,
                                        view->v4_video_delta.delta_bytes, 1)) {
        badge_rx_video_repair_observe_delta(view->v4_video_delta.group_id, view->v4_video_delta.group_index,
                                            view->v4_video_delta.delta_bytes, view->v4_video_delta.encoded_length);
        s_stats.v4_video_delta_count++;
        drain_cdg_to_idle(local_now_ms);
    } else {
        s_stats.cdg_delta_insert_fail++;
    }
}

static void handle_video_repair_window(const struct dashcdg_packet_view *view)
{
    const struct dashcdg_v4_repair_window_payload *rw;
    struct badge_rx_video_repair_group *g;
    uint16_t dir;

    if (view == NULL) {
        return;
    }
    rw = &view->v4_repair_window;
    if (rw->stream_type != DASHCDG_STREAM_TYPE_CDG) {
        return;
    }
    if (!(rw->repair_mode == DASHCDG_V4_REPAIR_MODE_VIDEO_WINDOW_XOR ||
          rw->repair_mode == DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY)) {
        return;
    }
    s_stats.v4_video_repair_rx_packets++;
    dir = (uint16_t)(rw->reserved & DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_MASK);
    if (dir == DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_FORWARD) {
        s_stats.v4_video_repair_rx_forward++;
    } else if (dir == DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_REVERSE) {
        s_stats.v4_video_repair_rx_reverse++;
    }
    g = badge_rx_video_repair_get_group(rw->group_id);
    if (g == NULL || rw->group_size <= 1U || rw->group_size > BADGE_RX_CDG_REPAIR_GROUP_SIZE || rw->payload_length == 0U ||
        rw->payload_length > BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX || rw->payload_bytes == NULL) {
        return;
    }
    if (g->expected_group_size == 0U || g->expected_group_size > rw->group_size) {
        g->expected_group_size = rw->group_size;
    }
    g->parity_present = 1U;
    dashcdg_fec_parity_init(&g->parity);
    g->parity.payload_bytes = rw->payload_length;
    g->parity.payload_length_xor = rw->payload_length;
    memcpy(g->parity.payload_xor, rw->payload_bytes, rw->payload_length);
    badge_rx_video_repair_try_recover_group(g);
}

static void rx_one_datagram(uint8_t *buf, size_t buflen, uint64_t local_now_ms)
{
    struct dashcdg_packet_view view;

    if (!dashcdg_protocol_parse_packet(&view, buf, buflen)) {
        s_stats.parse_failures++;
        return;
    }

    s_stats.last_sequence = view.header.sequence;
    {
        int64_t skew = (int64_t)view.header.sender_time_ms - (int64_t)local_now_ms;
        if (!s_stats.skew_ema_inited) {
            s_stats.skew_ema_ms = (int32_t)skew;
            s_stats.skew_ema_inited = 1;
        } else {
            s_stats.skew_ema_ms = (int32_t)((s_stats.skew_ema_ms * 7 + (int32_t)skew) / 8);
        }
    }

    switch (view.header.type) {
    case DASHCDG_PACKET_V4_SESSION_INFO:
        handle_session_info(&view);
        break;
    case DASHCDG_PACKET_V4_CLOCK_SYNC:
        handle_clock_sync(&view, local_now_ms);
        drain_cdg_to_idle(local_now_ms);
        break;
    case DASHCDG_PACKET_V4_LOADING_SCREEN:
        badge_rx_handle_v4_loading_screen(&view, local_now_ms);
        break;
    case DASHCDG_PACKET_V4_VIDEO_DELTA:
        handle_video_delta(&view, local_now_ms);
        break;
    case DASHCDG_PACKET_V4_REPAIR_WINDOW:
        handle_video_repair_window(&view);
        break;
    case DASHCDG_PACKET_V4_VIDEO_ANCHOR:
        s_stats.v4_anchor_chunks++;
        badge_rx_handle_v4_video_anchor(&view, local_now_ms);
        break;
    default:
        break;
    }
}

static void badge_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[DASHCDG_MAX_PACKET_SIZE];

    while (s_run) {
        fd_set rfds;
        struct timeval tv;
        int rv;
        int fd = (int)s_sock;

        if (fd < 0) {
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EBADF || errno == EINVAL || !s_run) {
                break;
            }
            ESP_LOGW(TAG, "select: errno=%d", errno);
            continue;
        }
        if (rv == 0 || !FD_ISSET(fd, &rfds)) {
            badge_rx_maybe_send_v4_stats(fd, dashcdg_clock_now_ms());
            continue;
        }

        for (;;) {
            struct sockaddr_in src;
            socklen_t srclen = (socklen_t)sizeof(src);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);
            if (n <= 0) {
                break;
            }
            if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (src.sin_family == AF_INET && badge_rx_ipv4_is_unicast_src(src.sin_addr.s_addr)) {
                s_v4_tx_src_ipv4 = src.sin_addr.s_addr;
            }
            s_stats.datagrams++;
            {
                uint64_t now_ms = dashcdg_clock_now_ms();
                rx_one_datagram(buf, (size_t)n, now_ms);
            }
            xSemaphoreGive(s_mtx);
        }
        badge_rx_maybe_send_v4_stats(fd, dashcdg_clock_now_ms());
    }

    ESP_LOGI(TAG, "rx task exit");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static int open_multicast_rx(void)
{
    int s;
    struct sockaddr_in addr;
    struct ip_mreq mr;
    int reuse = 1;
    int rcv = BADGE_RX_RCVBUF;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "socket errno=%d", errno);
        return -1;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BADGE_RX_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "bind errno=%d", errno);
        close(s);
        return -1;
    }

    memset(&mr, 0, sizeof(mr));
    mr.imr_multiaddr.s_addr = inet_addr(BADGE_RX_MCAST_ADDR);
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    {
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                mr.imr_interface.s_addr = ipi.ip.addr;
            }
        }
    }
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
        if (mr.imr_interface.s_addr != htonl(INADDR_ANY)) {
            /* STA IP may not be ready yet; retry default interface. */
            mr.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
                snprintf(s_stats.last_error, sizeof(s_stats.last_error), "IP_ADD_MEMBERSHIP errno=%d", errno);
                close(s);
                return -1;
            }
        } else {
            snprintf(s_stats.last_error, sizeof(s_stats.last_error), "IP_ADD_MEMBERSHIP errno=%d", errno);
            close(s);
            return -1;
        }
    }

    s_stats.igmp_joined = 1;
    return s;
}

void dashcdg_badge_rx_start(void)
{
    if (s_rx_task != NULL) {
        return;
    }

    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }

    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_stats.last_error, 0, sizeof(s_stats.last_error));
    memset(s_video_repair_groups, 0, sizeof(s_video_repair_groups));
    s_v4_tx_src_ipv4 = 0U;
    s_last_v4_stats_sent_ms = 0U;
    s_rx_stats_seq = 0U;

    dashcdg_media_clock_init(&s_mclk);
    s_jitter_cdg_primed = 0;
    s_last_cdg_apply_local_ms = 0U;
    s_announced_playout_delay_ms = 0U;
    s_sync_local_ms = 0U;
    s_sync_playback_ms = 0U;
    s_active_session_start_ms = 0U;
    s_active_asset_size = 0U;
    memset(s_active_song_id, 0, sizeof(s_active_song_id));
    s_active_session_valid = 0;

    s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    if (badge_rx_ensure_heap() != 0) {
        ESP_LOGW(TAG, "CDG/jitter unavailable — multicast RX, parse, clock sync still run");
    } else {
        dashcdg_cdg_state_init(s_cdg);
        dashcdg_cdg_batch_jitter_init(s_jb);
        dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
        if (badge_rx_ensure_blit_scratch() != 0) {
            ESP_LOGW(TAG, "CDG blit scratch OOM (~7 KiB) — decode+jitter ok, LCD band blits off until heap frees");
        } else {
            ESP_LOGI(TAG, "CDG+jitter static + blit scratch; v4 anchors decode into CDG (no extra decode heap)");
        }
    }

    s_sock = open_multicast_rx();
    if (s_sock < 0) {
        ESP_LOGE(TAG, "open_multicast_rx failed: %s", s_stats.last_error);
        badge_rx_free_heap();
        return;
    }

    s_run = 1;
    if (xTaskCreate(badge_rx_task, "badge_rx", BADGE_RX_STACK, NULL, BADGE_RX_TASK_PRIO, &s_rx_task) != pdPASS) {
        s_run = 0;
        close(s_sock);
        s_sock = -1;
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "xTaskCreate failed");
        ESP_LOGE(TAG, "%s", s_stats.last_error);
        badge_rx_free_heap();
    } else {
        ESP_LOGI(TAG, "listening UDP %s:%d (v4 CDG proof)", BADGE_RX_MCAST_ADDR, BADGE_RX_PORT);
    }
}

void dashcdg_badge_rx_stop(void)
{
    int fd;

    s_stats.igmp_joined = 0;
    s_run = 0;
    s_v4_tx_src_ipv4 = 0U;
    s_last_v4_stats_sent_ms = 0U;
    fd = (int)s_sock;
    if (fd >= 0) {
        s_sock = -1;
        close(fd);
    }
    for (int i = 0; i < 80 && s_rx_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_jb) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
    }
    /* Do not badge_rx_free_heap() here: keep pointers wired to static CDG/jitter across karaoke exits. */
    ESP_LOGI(TAG, "rx stopped");
}

void dashcdg_badge_rx_get_stats(dashcdg_badge_rx_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_jb) {
            s_stats.jb_next_packet_index = s_jb->next_packet_index;
            s_stats.jb_pending_slots = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
        } else {
            s_stats.jb_next_packet_index = 0;
            s_stats.jb_pending_slots = 0;
        }
        *out = s_stats;
        xSemaphoreGive(s_mtx);
    } else {
        *out = s_stats;
    }
    out->cdg_blit_max_y = s_cdg_blit_max_y;
    out->rx_task_running = (s_rx_task != NULL) ? 1U : 0U;
    snprintf(out->sta_ip, sizeof(out->sta_ip), "--");
    {
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                snprintf(out->sta_ip, sizeof(out->sta_ip), IPSTR, IP2STR(&ipi.ip));
            }
        }
    }
    out->cdg_heap_ok =
        (s_cdg != NULL && s_jb != NULL && s_cdg_blit_scratch != NULL) ? 1U : 0U;
    snprintf(out->tx_stats_dest, sizeof(out->tx_stats_dest), "--");
    if (s_v4_tx_src_ipv4 != 0U) {
        uint32_t h = ntohl(s_v4_tx_src_ipv4);
        snprintf(out->tx_stats_dest, sizeof(out->tx_stats_dest), "%u.%u.%u.%u", (unsigned)((h >> 24) & 0xffU),
                 (unsigned)((h >> 16) & 0xffU), (unsigned)((h >> 8) & 0xffU), (unsigned)(h & 0xffU));
    }
}

void dashcdg_badge_rx_format_mcast_modal(char *buf, size_t buf_sz)
{
    dashcdg_badge_rx_stats_t st;

    if (buf == NULL || buf_sz < 32) {
        return;
    }
    buf[0] = '\0';
    dashcdg_badge_rx_get_stats(&st);

    snprintf(buf, buf_sz,
             "--- Multicast / RX ---\n"
             "group ... %s:%d\n"
             "STA ip . %s\n"
             "IGMP ... %s\n"
             "task ... %s\n"
             "CDG decode %s\n"
             "stats→TX %s:%d (sent %lu)\n"
             "raster Y<=%u  jb_evict %lu  ins_fail %lu\n"
             "\n"
             "datagrams %llu\n"
             "parse_fail %lu\n"
             "v4 session %lu  clock %lu\n"
             "delta %lu  anchor %lu  load %lu\n"
             "rwin %lu  fwd %lu  rev %lu\n"
             "rrec %lu  rfail %lu\n"
             "seq %llu  skew_ema %ld ms\n"
             "have_clock %d\n"
             "song_id %s\n"
             "jb next %llu pend %u skips %llu\n"
             "\n"
             "%s\n"
             "TX must send v4 to this group/port.\n"
             "dg 0: no UDP (AP may filter mcast, wrong VLAN, or TX off-net).",
             BADGE_RX_MCAST_ADDR, BADGE_RX_PORT, st.sta_ip[0] ? st.sta_ip : "--", st.igmp_joined ? "joined" : "not joined",
             st.rx_task_running ? "running" : "stopped", st.cdg_heap_ok ? "on (bss+scratch)" : "off",
             (st.tx_stats_dest[0] && st.tx_stats_dest[0] != '-') ? st.tx_stats_dest : "(wire src?)",
             BADGE_RX_PORT, (unsigned long)st.v4_rx_stats_sent, (unsigned)st.cdg_blit_max_y,
             (unsigned long)st.jb_evict_rounds, (unsigned long)st.cdg_delta_insert_fail, (unsigned long long)st.datagrams,
             (unsigned long)st.parse_failures, (unsigned long)st.v4_session_count, (unsigned long)st.v4_clock_count,
             (unsigned long)st.v4_video_delta_count, (unsigned long)st.v4_anchor_chunks,
             (unsigned long)st.v4_loading_screen_count,
             (unsigned long)st.v4_video_repair_rx_packets, (unsigned long)st.v4_video_repair_rx_forward,
             (unsigned long)st.v4_video_repair_rx_reverse, (unsigned long)st.v4_video_repair_recovered,
             (unsigned long)st.v4_video_repair_failed, (unsigned long long)st.last_sequence, (long)st.skew_ema_ms,
             st.have_clock, st.song_id[0] ? st.song_id : "(none)",
             (unsigned long long)st.jb_next_packet_index, (unsigned)st.jb_pending_slots, (unsigned long long)st.live_missing_skips,
             st.last_error[0] ? st.last_error : "(no socket error)");
}

/** Absolute LVGL draw coords for the CDG slot (includes parent layout/padding/border offsets). */
static void badge_rx_cdg_lv_origin(lv_obj_t *o, int *ox, int *oy, int *cw, int *ch)
{
    lv_area_t a;

    lv_obj_get_coords(o, &a);
    *ox = (int)a.x1;
    *oy = (int)a.y1;
    *cw = (int)(a.x2 - a.x1 + 1);
    *ch = (int)(a.y2 - a.y1 + 1);
}

/** Caller holds `s_mtx`; `s_cdg` non-NULL; blit scratch ready. Consumes one `take_raster_dirty` pass. */
static void badge_rx_cdg_overlay_blit_current_dirty(lv_obj_t *cdg_lv_slot)
{
    static uint64_t s_last_overlay_keepalive_ms;
    int vx0 = 0;
    int vy0 = 0;
    int vx1 = 0;
    int vy1 = 0;
    dashcdg_cdg_raster_dirty_kind_t k = dashcdg_cdg_state_take_raster_dirty(s_cdg, &vx0, &vy0, &vx1, &vy1);

    if (k == DASHCDG_CDG_RASTER_DIRTY_NONE) {
        if (s_cdg_snapshots_applied > 0U || s_jitter_cdg_primed) {
            uint64_t now = dashcdg_clock_now_ms();
            if (now - s_last_overlay_keepalive_ms >= 160U) {
                dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
                s_last_overlay_keepalive_ms = now;
                k = dashcdg_cdg_state_take_raster_dirty(s_cdg, &vx0, &vy0, &vx1, &vy1);
            }
        }
        if (k == DASHCDG_CDG_RASTER_DIRTY_NONE) {
            return;
        }
    }
    {
        int ox;
        int oy;
        int cw;
        int ch;
        int clip_y = (int)s_cdg_blit_max_y;

        if (clip_y < 1) {
            clip_y = 1;
        }
        if (vy1 > clip_y) {
            vy1 = clip_y;
        }
        if (vy0 >= vy1) {
            return;
        }
        badge_rx_cdg_lv_origin(cdg_lv_slot, &ox, &oy, &cw, &ch);
        if (cw != DASHCDG_BADGE_RX_VISIBLE_W || ch != DASHCDG_BADGE_RX_VISIBLE_H) {
            ESP_LOGW(TAG, "CDG slot size %dx%d (expect %dx%d); blit may clip", cw, ch, DASHCDG_BADGE_RX_VISIBLE_W,
                     DASHCDG_BADGE_RX_VISIBLE_H);
        }
        for (int y = vy0; y < vy1;) {
            int bh = vy1 - y;
            if (bh > DASHCDG_BADGE_RX_BLIT_BAND_H) {
                bh = DASHCDG_BADGE_RX_BLIT_BAND_H;
            }
            {
                int vw = vx1 - vx0;
                dashcdg_badge_cdg_state_rect_to_rgb565_le(s_cdg, vx0, y, vw, bh, s_cdg_blit_scratch, (size_t)vw);
                if (dashcdg_display_blit_rgb565_lv_area(ox + vx0, oy + y, vw, bh, s_cdg_blit_scratch) != ESP_OK) {
                    ESP_LOGD(TAG, "panel blit failed @%d,%d %dx%d", ox + vx0, oy + y, vw, bh);
                }
                esp_rom_delay_us(BADGE_RX_PANEL_BAND_SETTLE_US);
            }
            y += bh;
        }
    }
}

void dashcdg_badge_rx_cdg_overlay_tick(lv_obj_t *cdg_lv_slot)
{
    if (cdg_lv_slot == NULL || !lv_obj_is_valid(cdg_lv_slot) || !dashcdg_display_lcd_panel()) {
        return;
    }

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (s_cdg == NULL) {
        xSemaphoreGive(s_mtx);
        return;
    }
    if (badge_rx_ensure_blit_scratch() != 0) {
        xSemaphoreGive(s_mtx);
        return;
    }

    badge_rx_cdg_overlay_blit_current_dirty(cdg_lv_slot);
    xSemaphoreGive(s_mtx);
}

void dashcdg_badge_rx_ui_tick(void *disp, void *lbl_stats, void *img_cdg, lv_obj_t *cdg_lv_slot)
{
    lv_obj_t *lbl = (lv_obj_t *)lbl_stats;

    (void)disp;
    (void)img_cdg;

    if (lbl != NULL && lv_obj_is_valid(lbl)) {
        dashcdg_badge_rx_stats_t st;
        char line[200];
        dashcdg_badge_rx_get_stats(&st);
        snprintf(line, sizeof(line), "dg %llu | seq %llu | (i) mcast", (unsigned long long)st.datagrams,
                 (unsigned long long)st.last_sequence);
        lv_label_set_text(lbl, line);
    }

    dashcdg_badge_rx_cdg_overlay_tick(cdg_lv_slot);
}
