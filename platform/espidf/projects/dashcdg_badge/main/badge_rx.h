#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define DASHCDG_BADGE_RX_VISIBLE_W 288
#define DASHCDG_BADGE_RX_VISIBLE_H 192
#define DASHCDG_BADGE_RX_RGB565_BYTES (DASHCDG_BADGE_RX_VISIBLE_W * DASHCDG_BADGE_RX_VISIBLE_H * sizeof(uint16_t))

/** Added to LVGL slot Y before `esp_lcd_panel_draw_bitmap` (optional trim; keep 0 when karaoke layout fits). */
#define DASHCDG_BADGE_RX_CDG_OVERLAY_BLIT_Y_NUDGE (0)

/** Panel band blit height (scratch = W * H * 2 bytes, no full framebuffer). */
#define DASHCDG_BADGE_RX_BLIT_BAND_H 12

typedef struct {
    uint64_t datagrams;
    uint32_t parse_failures;
    uint32_t v4_session_count;
    uint32_t v4_clock_count;
    uint32_t v4_video_delta_count;
    uint32_t v4_anchor_chunks;
    uint32_t v4_audio_chunk_rx;
    uint32_t v4_audio_frames_out;
    uint32_t v4_audio_decode_fail;
    uint32_t v4_audio_dac_begin_fail;
    uint32_t v4_audio_unsupported_codec;
    uint32_t v4_loading_screen_count;
    uint64_t last_sequence;
    int32_t skew_ema_ms;
    int skew_ema_inited;
    int have_clock;
    uint64_t jb_next_packet_index;
    size_t jb_pending_slots;
    uint64_t live_missing_skips;
    /** Estimated missing wire packets from header.sequence gaps (all packet types). */
    uint64_t wire_missing_estimate;
    /** Out-of-order/late wire sequence observations. */
    uint32_t wire_reorder_events;
    /** Per-stream gap estimates for quick corruption correlation in the modal. */
    uint32_t audio_missing_estimate;
    uint32_t cdg_missing_estimate;
    uint32_t clock_missing_estimate;
    uint32_t repair_missing_estimate;
    char song_id[80];
    char last_error[64];
    /** Filled in dashcdg_badge_rx_get_stats (IGMP join succeeded in open_multicast_rx). */
    uint8_t igmp_joined;
    /** RX worker task created and socket open (best-effort snapshot). */
    uint8_t rx_task_running;
    /** STA IPv4 from WIFI_STA_DEF, or "--" if unknown. */
    char sta_ip[20];
    /** CDG + jitter + blit scratch ready (0 = no full CDG path; RX/clock/sync may still run). */
    uint8_t cdg_heap_ok;
    /** Last v4 media sender unicast (PTP/rx-stats dest), or "--". */
    char tx_stats_dest[20];
    /** V4_RX_STATS datagrams successfully sent to TX PTP listener (BADGE_RX_TX_STATS_PORT). */
    uint32_t v4_rx_stats_sent;
    /** Peer V4_RX_STATS datagrams observed on BADGE_RX_TX_STATS_PORT. */
    uint32_t v4_rx_stats_peer_packets;
    /** Jitter backpressure: evict_pressure rounds (furthest-ahead batches dropped). */
    uint32_t jb_evict_rounds;
    /** CDG raster blit clipped to this height (192 full, 96 partial under jitter pressure). */
    uint16_t cdg_blit_max_y;
    /** Last insert after evict still failed (should be rare). */
    uint32_t cdg_delta_insert_fail;
    /** v4 repair-window packets observed for CDG stream (metrics-only ingest, no apply yet). */
    uint32_t v4_video_repair_rx_packets;
    uint32_t v4_video_repair_rx_forward;
    uint32_t v4_video_repair_rx_reverse;
    uint32_t v4_repair_nack_tx;
    uint32_t v4_video_repair_recovered;
    uint32_t v4_video_repair_failed;
    /** Runtime jitter capacities (slots). */
    uint16_t audio_slots_capacity;
    uint16_t video_slots_capacity;
    /** Active memory profile enum in badge_rx.c. */
    uint8_t memory_profile;
    /** Profile transitions and failed resize attempts. */
    uint32_t memory_profile_switches;
    uint32_t memory_profile_resize_failures;
    /** Frames dropped during shrink/resizes and adaptive grow count. */
    uint32_t memory_profile_resize_audio_dropped;
    uint32_t memory_profile_resize_video_dropped;
    uint32_t memory_profile_adaptive_grows;
    uint32_t memory_profile_adaptive_shrinks;
} dashcdg_badge_rx_stats_t;

void dashcdg_badge_rx_start(void);
void dashcdg_badge_rx_stop(void);
void dashcdg_badge_rx_set_decode_enabled(bool video_on, bool audio_on);
void dashcdg_badge_rx_get_decode_enabled(bool *video_on, bool *audio_on);
/** After LVGL freed widgets / heap settled, try calloc CDG+jitter again (no-op if already ok). */
void dashcdg_badge_rx_try_upgrade_cdg_heap(void);
void dashcdg_badge_rx_get_stats(dashcdg_badge_rx_stats_t *out);

/** Multi-line status for karaoke mcast modal (Wi-Fi, IGMP, wire counters). */
void dashcdg_badge_rx_format_mcast_modal(char *buf, size_t buf_sz);

/**
 * LVGL timer: optional stats label + CDG panel blit through `cdg_lv_slot` (see karaoke_ui).
 * Pass NULL for lbl_stats to skip label updates. Pass NULL for img_cdg (unused).
 */
void dashcdg_badge_rx_ui_tick(void *disp, void *lbl_stats, void *img_cdg, lv_obj_t *cdg_lv_slot);

/**
 * Direct panel RGB565 blit for CDG slot only (no stats label). Call from a short LVGL timer.
 * Do not call from LV_EVENT_FLUSH_FINISH on SPI + esp_lvgl_port: flush_finish runs before SPI DMA
 * completes, so overlapping draw_bitmap corrupts the panel stream.
 */
void dashcdg_badge_rx_cdg_overlay_tick(lv_obj_t *cdg_lv_slot);
