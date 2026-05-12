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

/** Inter-band panel settle after partial RGB565 blit (us); must match deferred blit worker. */
#define DASHCDG_BADGE_RX_PANEL_BAND_SETTLE_US 2200U

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
    /** Calls to degraded filler (hold-last + optional noise): decode/PLC/scratch failures — not “always on”. */
    uint32_t v4_audio_degraded_push;
    /** Audio jitter drain entered SKIP (holes → PLC/degraded); not the same as `v4_audio_decode_fail`. */
    uint32_t v4_audio_jitter_skip_events;
    uint32_t v4_audio_dac_begin_fail;
    uint32_t v4_audio_unsupported_codec;
    uint32_t v4_audio_codec_switches;
    uint32_t v4_audio_codec_mismatch;
    uint32_t v4_loading_screen_count;
    uint64_t last_sequence;
    int32_t skew_ema_ms;
    int skew_ema_inited;
    int have_clock;
    uint64_t jb_next_packet_index;
    size_t jb_pending_slots;
    /** Audio jitter occupied slots (same instant as jb_pending_slots snapshot). */
    uint32_t audio_jb_pending_slots;
    uint64_t live_missing_skips;
    /** Estimated missing wire packets from header.sequence gaps (all packet types). */
    uint64_t wire_missing_estimate;
    /** Out-of-order/late wire sequence observations. */
    uint32_t wire_reorder_events;
    /** Per-stream gap estimates for quick corruption correlation in the modal. */
    uint32_t audio_missing_estimate;
    uint32_t cdg_missing_estimate;
    /** CDG hard-resync skips (large cursor jumps) and total packets skipped by those events. */
    uint32_t cdg_hard_resync_events;
    uint32_t cdg_hard_resync_packets;
    /** Sender-playback minus CDG jitter playback cursor (0 when unavailable). */
    int32_t cdg_lag_ms;
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
    /** Insert dropped duplicate batch (same packet_start_index). */
    uint32_t cdg_delta_insert_dup;
    /** Insert dropped stale batch entirely before apply cursor (normal under repair/STA dup reorder). */
    uint32_t cdg_delta_insert_stale;
    /** v4 repair-window packets observed for CDG stream (metrics-only ingest, no apply yet). */
    uint32_t v4_video_repair_rx_packets;
    uint32_t v4_video_repair_rx_forward;
    uint32_t v4_video_repair_rx_reverse;
    uint32_t v4_repair_nack_tx;
    /** repair-nack send attempts (includes coalesced / failed sends). */
    uint32_t v4_repair_nack_attempt;
    /** sendto failures or short writes when sending repair-nack. */
    uint32_t v4_repair_nack_send_fail;
    /** Skipped sends due to adaptive min-spacing throttle (RSSI-aware when enabled in Kconfig). */
    uint32_t v4_repair_nack_throttled;
    /** v4 RLE anchors not applied because packet_index trailed jitter cursor beyond slack. */
    uint32_t v4_anchor_rejected_behind;
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
    /** NVS: request CDG repair parity from TX (v4_repair_nack). */
    uint8_t repair_nack_enabled;
    /** NVS: send periodic v4_rx_stats to TX listener (reduces multicast airtime when off). */
    uint8_t v4_stats_tx_enabled;
    /** IGMP + bind succeeded on CDG FEC repair port (24686 when TX uses split repair). */
    uint8_t v4_repair_rx_socket_ok;
    /** Dedicated send-only socket for v4_rx_stats + repair-nack (never the media 24684 socket). */
    uint8_t v4_control_uplink_ok;
    /** Unicast-duplicate RX: bit0=media bit1=stats bit2=repair (STA bind ok). */
    uint8_t ucast_rx_mask;
    /** Media datagrams seen on multicast and optional STA unicast-dup media sockets. */
    uint32_t mcast_media_datagrams;
    uint32_t ucast_media_datagrams;
    /** Exact duplicate wire-sequence observations on the media path (likely mcast+ucast double-delivery). */
    uint32_t media_sequence_duplicate_hits;
    /** v4_rx_stats sends skipped by startup/recovery/pressure throttling policy. */
    uint32_t v4_rx_stats_suppressed;
    /** Audio chunks arrived while no DAC push succeeded for >500 ms (warning event count). */
    uint32_t audio_rx_no_dac_warn;
    /** Media path policy: 0=both, 1=mcast_prefer, 2=ucast_prefer, 3=auto. */
    uint8_t media_path_policy;
    /** Karaoke settings snapshot: affects HUD (audio-only must not require CDG heap). */
    uint8_t video_decode_enabled;
    uint8_t audio_decode_enabled;
    /** RX pump: xSemaphoreTake(s_mtx) hit timeout (datagram dropped that iteration). */
    uint32_t rx_mtx_pump_timeouts;
    /** Repair recv loops: s_mtx take timeout count. */
    uint32_t rx_mtx_repair_timeouts;
    /** CDG overlay: band not queued (pool/work queue full or non-blocking slot wait). */
    uint32_t cdg_blit_band_drop;
    /** Max observed s_mtx hold (us) in media pump one datagram (0 if CONFIG_DASHCDG_BADGE_PERF_PROBES off). */
    uint32_t perf_mtx_pump_max_us;
    /** Max s_mtx hold (us) CDG overlay RGB565 prep when deferred blit enabled. */
    uint32_t perf_mtx_overlay_rgb_max_us;
    /** Max SPI band (blit + settle) on deferred worker (us). */
    uint32_t perf_sp_blit_band_max_us;
    /** RX idle-select path: failed to take `s_mtx` in time — audio drain skipped (chop risk). */
    uint32_t rx_mtx_idle_drain_miss;
    /** After UDP recv burst: failed to take `s_mtx` for post-burst drain — chop risk. */
    uint32_t rx_mtx_postburst_drain_miss;
    /** LVGL tick cooperative audio-only drains that ran (bounded budget). */
    uint32_t lvgl_coop_audio_drains;
    /** RX owner command queue: commands dropped because the queue was full (zero-tick send). */
    uint32_t rx_cmd_q_drops;
    /** RX owner command queue: peak slots used (high-water mark observed at drain time). */
    uint16_t rx_cmd_q_high_water;
    /** RX owner command queue: total commands dequeued and applied by the RX task. */
    uint32_t rx_cmd_q_applied;
} dashcdg_badge_rx_stats_t;

#define DASHCDG_BADGE_UCAST_RX_MASK_MEDIA  0x01U
#define DASHCDG_BADGE_UCAST_RX_MASK_STATS  0x02U
#define DASHCDG_BADGE_UCAST_RX_MASK_REPAIR 0x04U

void dashcdg_badge_rx_start(void);
void dashcdg_badge_rx_stop(void);
/** True while the multicast/UDP RX task is running (karaoke / lab RX). Used to suppress auto panel sleep. */
bool dashcdg_badge_rx_is_running(void);
/**
 * After DHCP (IP_EVENT_STA_GOT_IP): re-open STA unicast UDP dup binds + re-join IGMP on mcast fds.
 * If karaoke RX is not running yet, sets an internal flag so the next dashcdg_badge_rx_start() does
 * extra IGMP + unicast bootstrap (_STA_GOT_IP cannot join without sockets).
 */
void dashcdg_badge_rx_notify_sta_got_ip(void);
void dashcdg_badge_rx_set_decode_enabled(bool video_on, bool audio_on);
void dashcdg_badge_rx_get_decode_enabled(bool *video_on, bool *audio_on);
/** Reload NVS tuning flags (repair NACK, v4 stats uplink). Safe before/during RX task. */
void dashcdg_badge_rx_apply_rx_tuning_prefs(void);
/** After LVGL freed widgets / heap settled, try calloc CDG+jitter again (no-op if already ok). */
void dashcdg_badge_rx_try_upgrade_cdg_heap(void);
void dashcdg_badge_rx_get_stats(dashcdg_badge_rx_stats_t *out);
/**
 * LVGL tick helper (T4): one `s_mtx` acquisition — refresh `s_stats` mirror, copy to `out`, optional CDG
 * overlay blit (same rules as `dashcdg_badge_rx_cdg_overlay_tick`). Call from the LVGL task only.
 */
void dashcdg_badge_rx_lvgl_overlay_tick_and_get_stats(lv_obj_t *cdg_lv_slot, dashcdg_badge_rx_stats_t *out);
/** Deferred SPI blit worker: record one band blit+settle duration (us) for perf max. */
void dashcdg_badge_rx_perf_note_sp_blit_band_us(uint32_t band_spi_us);

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
