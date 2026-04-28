#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif

#if defined(DASHCDG_RX_UI_GDI_ONLY)
#if !defined(_WIN32)
#error "DASHCDG_RX_UI_GDI_ONLY is only supported on Windows (Win32 GDI receiver)"
#endif
#define DASHCDG_RX_HAVE_GLUT 0
#else
#define DASHCDG_RX_HAVE_GLUT 1
#endif

#if DASHCDG_RX_HAVE_GLUT
#include <GL/glew.h>
#include <GL/glut.h>
#endif

#include "dashcdg/app_modes.h"
#include "dashcdg/audio_jitter.h"
#include "dashcdg/cdg_batch_jitter.h"
#include "dashcdg/cdg.h"
#include "dashcdg/cdg_raster.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/desktop_async_log.h"
#include "dashcdg/fec.h"
#if DASHCDG_RX_HAVE_GLUT
#include "dashcdg/gl_renderer.h"
#endif
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/pcm_soxr_stream.h"
#include "dashcdg/pcm_rate_convert.h"
#include "dashcdg/protocol.h"
#include "dashcdg/amr_codec.h"
#include "dashcdg/nb_ima_codec.h"
#include "dashcdg/nb_codec_adapters.h"
#include "dashcdg/stream_runtime.h"
#include "dashcdg/transport_udp.h"
#include "dashcdg/win32_timing_boost.h"

#ifndef DASHCDG_BUILD_VERSION
#define DASHCDG_BUILD_VERSION "dev-unknown-gunknown"
#endif

/*
 * MinGW32 builds use -D__USE_MINGW_ANSI_STDIO=0 (see Makefile), so printf-style
 * functions resolve to Windows XP msvcrt.dll. That CRT does not support C99
 * %zu / %ll length modifiers reliably; using them corrupts the va_list and can
 * fault inside vsnprintf (observed when toggling the on-screen HUD).
 */
#ifdef _WIN32
#define DASHCDG_RX_PRIu64 "I64u"
#define DASHCDG_RX_PRIi64 "I64d"
#else
#define DASHCDG_RX_PRIu64 "llu"
#define DASHCDG_RX_PRIi64 "lld"
#endif

#if DASHCDG_RX_HAVE_GLUT
#ifdef _WIN32
#include "dashcdg/win32_gdi_view.h"
#endif
#else
#include "dashcdg/win32_gdi_view.h"
#endif

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 2U
/* Matches TX `DASHCDG_PAYOUT_DELAY_MS` / v4 default preroll when session_info not yet received. */
#define DASHCDG_RX_V4_BOOTSTRAP_PLAYOUT_DELAY_MS 500U
#define DASHCDG_AUDIO_LATE_GRACE_MS 80U
#define DASHCDG_CDG_LATE_GRACE_MS 120U
#define DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE 16U
#define DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX 8U
#define DASHCDG_TRACKED_FEC_GROUPS 32U
#define DASHCDG_MAX_DRAIN_STEPS_PER_CALL 256U
#define DASHCDG_MAX_PTP_EXCHANGE_AGE_MS 500U
#define DASHCDG_RENDER_FRAME_INTERVAL_MS 20U
/* No UDP for this long → show reconnecting overlay (between typical 15–20 s loss UX targets). */
#define DASHCDG_STREAM_LOSS_RECONNECT_MS 18000U
#define DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS 2000U
#define DASHCDG_RX_STATS_SUMMARY_INTERVAL_MS 2000U
#define DASHCDG_RX_REPAIR_NACK_COOLDOWN_MS 120U
#define DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS 480U
#define DASHCDG_RX_OUTPUT_LATENCY_FALLBACK_MS 120U
#define DASHCDG_RX_APP_RING_SAFETY_MS 40U
#define DASHCDG_RX_MIN_APP_RING_TARGET_MS 120U
/*
 * Before the first Pa_StartStream, require at most this much app-ring PCM in
 * dashcdg_rx_claim_audio_start_locked. v4 bootstrap playout delay (500 ms) yields a ~250 ms target from
 * announced_playout_delay/2 — that can wedge cold join while CDG/graphics advance.
 */
#define DASHCDG_RX_CLAIM_AUDIO_START_BUFFER_CAP_MS 340U
#define DASHCDG_RX_MAX_APP_RING_TARGET_MS 500U
#define DASHCDG_RX_APP_RING_HEADROOM_MS 180U
#define DASHCDG_RX_MIN_RING_CAPACITY_MS 180U
#define DASHCDG_RX_MAX_RING_CAPACITY_MS 900U
#define DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS 4
#define DASHCDG_RX_QUEUE_SERVO_GAIN_PPM_PER_MS 3
#define DASHCDG_RX_QUEUE_SERVO_MAX_PPM 320
#define DASHCDG_RX_QUEUE_SERVO_FINE_GAIN_PPM_PER_MS 1
#define DASHCDG_RX_QUEUE_SERVO_FINE_BAND_MS 10
#define DASHCDG_RX_QUEUE_SERVO_HOLD_UPDATE_MS 120U
#define DASHCDG_RX_QUEUE_SERVO_WARMUP_MS 1800U
#define DASHCDG_RX_QUEUE_SERVO_WARMUP_SYNC_ACTIVE_MS 450U
#define DASHCDG_RX_QUEUE_SERVO_BACKPRESSURE_HOLD_MS 900U
#define DASHCDG_RX_ZERO_BUFFER_STALL_RECOVER_MS 750U
#define DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS 900U
#define DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS 3000U
#define DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_EVENTS 2U
#define DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS 350U
#define DASHCDG_RX_HOST_UNDERRUN_RECOVER_MAX_BUFFER_MS 120U
/*
 * WASAPI / Win11: Pa_IsStreamActive often blips inactive for one media tick (~10 ms). Requiring a run
 * of inactive observations before dashcdg_rx_handle_dead_audio_backend_locked rebuilds avoids ripping
 * down a live stream shortly after reopen (heard as ~1–3 s of audio then silence).
 */
#if DASHCDG_HAVE_PORTAUDIO
#define DASHCDG_RX_PA_DEAD_BACKEND_MIN_STREAK 300U
#define DASHCDG_RX_PA_DEAD_BACKEND_MIN_STALE_MS 2500U
#endif
#define DASHCDG_RX_SOURCE_IDLE_PARK_MS 5000U
#define DASHCDG_RX_DECODE_STALL_RECOVER_MS 1400U
#define DASHCDG_RX_DECODE_STALL_MIN_PENDING_SLOTS 4U
#define DASHCDG_RX_POST_TRACK_RECOVER_WINDOW_MS 15000U
#define DASHCDG_RX_POST_TRACK_RECOVER_STALE_MS 1100U
#define DASHCDG_RX_LEADER_BIAS_HOLDOFF_AFTER_TRACK_MS 1200U
#define DASHCDG_TX_GROUP_SYNC_MODE_OFF 0U
#define DASHCDG_TX_GROUP_SYNC_MODE_MEASURE 1U
#define DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE 2U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MAGIC 0xA0000000U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MAGIC_MASK 0xF0000000U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MODE_SHIFT 26U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MODE_MASK 0x3U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_TARGET_SHIFT 16U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_TARGET_MASK 0x3FFU
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_SHIFT 8U
#define DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_MASK 0xFFU
/*
 * v4 stale-media filter compares header.sender_time_ms + preroll to session_start_ms. TX sets
 * session_start_ms to the playback anchor, which can be now_ms + TX warmup — larger than preroll —
 * so legitimate first-track / new-track packets were misclassified as prior-session and dropped
 * until wall time caught up (long frozen video + audio, then a burst / "fast-forward" catch-up).
 * When session_start_ms is ahead of the session_info datagram's sender time, remember that floor
 * and only treat packets as stale if they are clearly older than that envelope (with reorder slack).
 */
#define DASHCDG_RX_V4_SESSION_REORDER_SENDER_SLACK_MS 100U
/*
 * WinMM (waveOut) builds — typical for GDI-only / legacy desktop-rx — expose coarser host
 * underrun counters and less steady timestamp_ms than PortAudio. Default 750/900 ms stall gates
 * plus a 3 s post-recover cooldown made auto-recover feel stuck versus GL+PortAudio player builds.
 */
#if defined(_WIN32) && defined(DASHCDG_DESKTOP_WIN32_WAVEOUT) && (DASHCDG_DESKTOP_WIN32_WAVEOUT)
#undef DASHCDG_RX_ZERO_BUFFER_STALL_RECOVER_MS
#define DASHCDG_RX_ZERO_BUFFER_STALL_RECOVER_MS 480U
#undef DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS
#define DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS 600U
#undef DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS
#define DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS 1200U
#endif
#if defined(DASHCDG_RX_UI_GDI_ONLY)
#define DASHCDG_RX_RENDER_SNAPSHOT_INTERVAL_MS 20U
#else
#define DASHCDG_RX_RENDER_SNAPSHOT_INTERVAL_MS 10U
#endif
#define DASHCDG_CDG_SNAPSHOT_STATE_BYTES (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U) + \
        (DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT))
#define DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT ((DASHCDG_CDG_SNAPSHOT_STATE_BYTES + DASHCDG_MAX_CDG_SNAPSHOT_CHUNK - 1U) / \
        DASHCDG_MAX_CDG_SNAPSHOT_CHUNK)

#ifdef _WIN32
static int dashcdg_rx_should_use_legacy_recovery_fallback(void) {
    static int cached = -1;

    if (cached >= 0) {
        return cached;
    }
    {
#if defined(DASHCDG_DESKTOP_WIN32_WAVEOUT) && (DASHCDG_DESKTOP_WIN32_WAVEOUT)
        /*
         * WaveOut has coarser running/timestamp semantics and benefits from explicit stop+flush
         * recovery (re-prime path) rather than resume-style jitter reset.
         */
        cached = 1;
        return cached;
#endif
        OSVERSIONINFOA vi;

        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (!GetVersionExA(&vi)) {
            cached = 0;
        } else {
            cached = vi.dwMajorVersion < 6U;
        }
    }
    return cached;
}
#else
static int dashcdg_rx_should_use_legacy_recovery_fallback(void) {
    return 0;
}
#endif
/*
 * TX currently paces v4 anchors in 512-byte chunks to avoid large startup bursts. RX must track
 * completion using the same stride; indexing by the protocol max (1024) collapses adjacent anchor
 * chunks onto the same slot and late joins never finish assembling the first bridge canvas.
 */
#define DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE 512U
#define DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES (4U + (DASHCDG_CDG_SNAPSHOT_STATE_BYTES * 2U))
#define DASHCDG_V4_ANCHOR_CHUNK_COUNT ((DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES + DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE - 1U) / \
        DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE)
#define DASHCDG_RX_AUDIO_ARRIVAL_GAP_THRESHOLD_MS 80U
#define DASHCDG_RX_AUDIO_ARRIVAL_BURST_THRESHOLD_MS 3U
#if defined(DASHCDG_RX_UI_GDI_ONLY)
#define DASHCDG_RX_AUDIO_ARRIVAL_FAULT_LOG_MIN_MS 2000U
#else
#define DASHCDG_RX_AUDIO_ARRIVAL_FAULT_LOG_MIN_MS 500U
#endif
#define DASHCDG_RX_AUDIO_HARD_RESYNC_LOG_COOLDOWN_MS 1000U
#define DASHCDG_RX_SYNC_LEADER_BIAS_STALE_MS 3000U
#define DASHCDG_RX_SYNC_LEADER_BIAS_PPM_MAX 80
#define DASHCDG_RX_HUD_COLOR_OK_RGB 0x78F078U
#define DASHCDG_RX_HUD_COLOR_WARN_RGB 0xFFD060U
#define DASHCDG_RX_HUD_COLOR_ERR_RGB 0xFF6A6AU
#define DASHCDG_RX_HUD_GSPREAD_WARN_MS 25U
#define DASHCDG_RX_HUD_GSPREAD_ERR_MS 60U

static void dashcdg_frame_limit_wait(uint64_t *next_deadline_ms, uint32_t frame_interval_ms) {
    uint64_t now_ms;

    if (next_deadline_ms == NULL || frame_interval_ms == 0U) {
        return;
    }

    now_ms = dashcdg_clock_now_ms();
    if (*next_deadline_ms == 0U) {
        *next_deadline_ms = now_ms + (uint64_t) frame_interval_ms;
        return;
    }
    if (now_ms < *next_deadline_ms) {
        dashcdg_sleep_ms((unsigned int) (*next_deadline_ms - now_ms));
        now_ms = dashcdg_clock_now_ms();
    }
    if (now_ms > *next_deadline_ms + (uint64_t) frame_interval_ms * 4ULL) {
        *next_deadline_ms = now_ms + (uint64_t) frame_interval_ms;
    } else {
        *next_deadline_ms += (uint64_t) frame_interval_ms;
    }
}

static const struct {
    char c;
    uint8_t rows[7];
} g_dashcdg_connect_font[] = {
        { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
        { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
        { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E } },
        { 'I', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F } },
        { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
        { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
        { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }
};

struct dashcdg_rx_fec_group {
    int occupied;
    uint32_t group_id;
    uint8_t expected_group_size;
    int parity_present;
    uint8_t parity_present_mask;
    uint8_t parity_symbol_bytes;
    struct dashcdg_fec_parity_state parity;
    uint8_t parity_symbols[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    uint8_t member_present[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint16_t member_lengths[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t member_payloads[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE][DASHCDG_MAX_FEC_PAYLOAD_BYTES];
};

struct dashcdg_rx_render_snapshot {
    int valid;
    int playback_ms;
    struct dashcdg_cdg_state state;
};

struct receiver_state {
    pthread_mutex_t mutex;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_media_clock sender_clock;
    uint8_t *asset_bytes;
    uint8_t *chunk_seen;
    size_t asset_size;
    size_t chunk_size;
    size_t chunk_count;
    size_t received_chunks;
    uint64_t session_start_ms;
    uint64_t v4_session_epoch_anchor_sender_ms;
    uint64_t playback_base_ms;
    uint64_t playback_base_sender_ms;
    uint64_t last_audio_jitter_apply_local_ms;
    uint64_t last_audio_queue_success_local_ms;
    uint64_t last_audio_timestamp_advance_local_ms;
    uint64_t last_cdg_jitter_apply_local_ms;
    uint64_t audio_skip_hold_until_local_ms;
    uint64_t cdg_skip_hold_until_local_ms;
    char song_id[DASHCDG_MAX_SONG_ID];
    size_t contiguous_prefix_chunks;
    uint64_t datagrams_received;
    uint64_t bytes_received;
    uint64_t parse_failures;
    uint64_t announce_packets;
    uint64_t asset_chunk_packets;
    uint64_t clock_beacon_packets;
    uint64_t unknown_packets;
    uint64_t duplicate_chunks;
    uint64_t asset_bytes_written;
    uint64_t last_progress_local_ms;
    uint64_t last_datagram_local_ms;
    uint32_t rx_interarrival_jitter_ema_ms;
    int rx_sender_skew_ema_inited;
    int64_t rx_sender_skew_ema_ms;
    uint32_t rx_stats_report_seq;
    uint64_t rx_stats_last_sent_local_ms;
    uint64_t rx_stats_last_summary_local_ms;
    uint64_t v4_rx_stats_peer_packets;
    uint64_t audio_packets;
    uint64_t cdg_batch_packets;
    uint64_t ptp_sync_packets;
    uint64_t ptp_follow_up_packets;
    uint64_t ptp_delay_req_packets;
    uint64_t ptp_delay_resp_packets;
    uint64_t v4_session_info_packets;
    uint64_t v4_loading_screen_packets;
    uint64_t v4_video_anchor_packets;
    uint64_t v4_audio_chunk_packets;
    uint64_t v4_video_delta_packets;
    uint64_t v4_repair_window_packets;
    uint64_t v4_clock_sync_packets;
    uint64_t live_packets_applied;
    uint64_t audio_decode_failures;
    uint64_t audio_queue_overflows;
    uint64_t audio_missing_skips;
    uint64_t audio_hard_resync_events;
    uint64_t live_missing_skips;
    uint64_t fec_packets;
    uint64_t fec_audio_packets;
    uint64_t fec_cdg_packets;
    uint64_t cdg_snapshot_packets;
    uint64_t cdg_snapshots_applied;
    uint64_t fec_audio_recovered;
    uint64_t fec_cdg_recovered;
    uint64_t fec_recovery_failures;
    uint64_t cdg_unrecoverable_groups;
    uint64_t repair_nack_tx;
    uint64_t last_logged_audio_queue_overflows;
    uint64_t last_logged_audio_missing_skips;
    uint64_t last_logged_audio_hard_resync_events;
    uint64_t last_logged_live_missing_skips;
    uint64_t last_logged_audio_reordered_packets;
    uint64_t last_logged_cdg_reordered_batches;
    uint64_t last_logged_stream_underrun_events;
    uint64_t last_observed_stream_underrun_events;
    uint64_t last_observed_stream_underrun_frames;
    uint64_t last_logged_host_underrun_fault_local_ms;
    uint64_t audio_arrival_gap_events;
    uint64_t audio_arrival_gap_max_ms;
    uint64_t audio_arrival_burst_events;
    uint64_t audio_arrival_burst_max_run;
    uint64_t audio_last_chunk_local_ms;
    uint64_t audio_burst_window_start_local_ms;
    uint32_t audio_burst_run_count;
    uint64_t last_audio_hard_resync_local_ms;
    uint64_t last_logged_audio_arrival_gap_events;
    uint64_t last_logged_audio_arrival_burst_events;
    uint64_t last_logged_audio_arrival_fault_local_ms;
    uint16_t sync_leader_instance_id_low16;
    int16_t sync_leader_trim_bias_ppm;
    uint64_t sync_leader_last_update_local_ms;
    uint8_t sync_group_mode;
    uint16_t sync_group_target_latency_ms;
    uint8_t sync_group_phase_spread_ms;
    uint16_t announced_audio_sample_rate;
    uint8_t announced_audio_channels;
    uint16_t announced_playout_delay_ms;
    uint8_t announced_audio_frame_ms;
    uint8_t announced_transport_version;
    uint8_t announced_audio_profile_id;
    uint8_t announced_audio_codec_id;
    uint8_t announced_audio_fec_group_size;
    uint8_t announced_cdg_fec_group_size;
    uint16_t rx_audio_applied_wire_sr;
    uint8_t rx_audio_applied_wire_ch;
    uint8_t rx_audio_applied_frame_ms;
    uint16_t rx_audio_applied_preroll_ms;
    uint8_t rx_audio_applied_profile_id;
    uint8_t rx_audio_applied_codec_id;
    int rx_audio_applied_valid;
    int last_audio_timestamp_ms;
    uint32_t pending_sync_id;
    uint64_t pending_sync_rx_local_ms;
    uint64_t pending_sync_origin_remote_ms;
    uint32_t next_delay_request_id;
    uint32_t pending_delay_request_id;
    uint64_t pending_delay_request_local_ms;
    int64_t sender_offset_ms;
    int64_t sender_path_delay_ms;
    int64_t sender_offset_step_ms;
    int64_t sender_path_step_ms;
    int64_t sender_offset_jitter_peak_ms;
    int64_t sender_path_jitter_peak_ms;
    uint64_t sender_clock_updates;
    uint64_t ptp_exchange_successes;
    uint64_t ptp_fallback_updates;
    uint64_t last_clock_update_local_ms;
    struct dashcdg_cdg_state live_state;
    struct dashcdg_cdg_state v4_bridge_cdg;
    int v4_bridge_cdg_valid;
    int reader_ready;
    int have_clock;
    int playback_paused;
    int network_audio_enabled;
    int pending_sync_valid;
    int pending_delay_request_valid;
    uint32_t active_snapshot_id;
    uint64_t active_snapshot_packet_index;
    uint32_t active_snapshot_total_bytes;
    size_t active_snapshot_received_bytes;
    size_t active_snapshot_received_chunks;
    uint8_t active_snapshot_bytes[DASHCDG_CDG_SNAPSHOT_STATE_BYTES];
    uint8_t active_snapshot_chunk_seen[DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT];
    uint32_t active_v4_anchor_id;
    uint64_t active_v4_anchor_packet_index;
    uint32_t active_v4_anchor_total_bytes;
    size_t active_v4_anchor_received_bytes;
    size_t active_v4_anchor_received_chunks;
    uint8_t active_v4_anchor_bytes[DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES];
    uint8_t active_v4_anchor_chunk_seen[DASHCDG_V4_ANCHOR_CHUNK_COUNT];
    uint8_t v4_loading_screen_kind;
    uint8_t v4_loading_screen_phase;
    int v4_loading_screen_active;
    struct dashcdg_audio_jitter_buffer audio_jitter;
    struct dashcdg_cdg_batch_jitter_buffer cdg_batch_jitter;
    int jitter_audio_decode_primed;
    int jitter_cdg_decode_primed;
    struct dashcdg_rx_fec_group audio_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
    struct dashcdg_rx_fec_group cdg_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
    int16_t pcm_src_overlap_l[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    int16_t pcm_src_overlap_r[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t pcm_src_overlap_valid;
    uint64_t pcm_src_stream_in_samples;
    uint64_t pcm_src_stream_out_samples;
    uint32_t audio_target_total_latency_ms;
    uint32_t audio_target_buffer_ms;
    uint32_t audio_ring_capacity_ms;
    uint32_t audio_host_output_latency_ms;
    int32_t audio_resample_trim_ppm;
    int64_t audio_equal_rate_slip_accum;
    uint64_t audio_servo_enable_after_local_ms;
    uint64_t audio_last_servo_update_local_ms;
    uint64_t audio_last_queue_pressure_local_ms;
    uint64_t audio_last_stall_recover_local_ms;
    uint64_t last_session_change_local_ms;
    uint64_t recovery_host_underrun_count;
    uint64_t recovery_zero_buffer_count;
    uint64_t recovery_silent_stall_count;
    uint64_t source_idle_park_count;
};

#if defined(DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED) && defined(DASHCDG_AUDIO_JITTER_HEAP_BACKED)
/*
 * Desktop heap-backed jitter: grow/shrink policy aligned with
 * `badge_rx_adapt_jitter_capacity_locked` (ESP32). No ESP heap_caps budget — caps use hard maxima.
 */
#define DASHCDG_RX_JB_ADAPT_TUNE_MS 250U
#define DASHCDG_RX_JB_ADAPT_STEP_SLOTS 8U
#define DASHCDG_RX_JB_ADAPT_STEP_FAST_SLOTS 24U
#define DASHCDG_RX_JB_ADAPT_OCC_PCT 65U
#define DASHCDG_RX_JB_ADAPT_OCC_PCT_FAST 80U
#define DASHCDG_RX_JB_ADAPT_SHRINK_OCC_PCT 20U
#define DASHCDG_RX_JB_ADAPT_SHRINK_HOLDOFF_MS 12000U
#define DASHCDG_RX_JB_ADAPT_BURST_OCC_PCT 40U
#define DASHCDG_RX_JB_ADAPT_BURST_HOLD_MS 2200U
#define DASHCDG_RX_JB_ADAPT_FAIL_COOLDOWN_MS 2500U
#define DASHCDG_RX_JB_ADAPT_FAIL_STREAK_STEP_DOWN 3U
#define DASHCDG_RX_AUDIO_JB_BALANCED_SLOTS DASHCDG_AUDIO_JITTER_SLOT_COUNT
#define DASHCDG_RX_AUDIO_JB_HARD_MAX_SLOTS 192U
#define DASHCDG_RX_CDG_JB_BALANCED_SLOTS DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT
#define DASHCDG_RX_CDG_JB_HARD_MAX_SLOTS 256U

static uint64_t s_rx_jb_adapt_last_tune_ms;
static uint64_t s_rx_jb_adapt_last_grow_ms;
static uint64_t s_rx_jb_adapt_last_burst_cdg_ms;
static uint64_t s_rx_jb_adapt_last_burst_audio_ms;
static uint64_t s_rx_jb_adapt_last_resize_fail_ms;
static uint8_t s_rx_jb_adapt_fail_streak;
static uint64_t s_rx_jb_adapt_last_cdg_pending_drops;
static uint64_t s_rx_jb_adapt_last_audio_pending_drops;
static uint64_t s_rx_jb_adapt_last_cdg_reordered;

static void dashcdg_rx_jitter_adapt_reset_session(void) {
    s_rx_jb_adapt_last_tune_ms = 0U;
    s_rx_jb_adapt_last_grow_ms = 0U;
    s_rx_jb_adapt_last_burst_cdg_ms = 0U;
    s_rx_jb_adapt_last_burst_audio_ms = 0U;
    s_rx_jb_adapt_last_resize_fail_ms = 0U;
    s_rx_jb_adapt_fail_streak = 0U;
    s_rx_jb_adapt_last_cdg_pending_drops = 0U;
    s_rx_jb_adapt_last_audio_pending_drops = 0U;
    s_rx_jb_adapt_last_cdg_reordered = 0U;
}

static void dashcdg_rx_jitter_heap_init(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    dashcdg_audio_jitter_init(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_init(&state->cdg_batch_jitter);
}

static void dashcdg_rx_jitter_heap_shutdown(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    dashcdg_audio_jitter_release(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_release(&state->cdg_batch_jitter);
}

static void dashcdg_rx_jitter_shrink_to_balanced_on_reset(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    if (dashcdg_audio_jitter_capacity(&state->audio_jitter) > DASHCDG_RX_AUDIO_JB_BALANCED_SLOTS) {
        (void) dashcdg_audio_jitter_resize(&state->audio_jitter, DASHCDG_RX_AUDIO_JB_BALANCED_SLOTS);
    }
    if (dashcdg_cdg_batch_jitter_capacity(&state->cdg_batch_jitter) > DASHCDG_RX_CDG_JB_BALANCED_SLOTS) {
        (void) dashcdg_cdg_batch_jitter_resize(&state->cdg_batch_jitter, DASHCDG_RX_CDG_JB_BALANCED_SLOTS);
    }
}

static void dashcdg_rx_adapt_jitter_capacity_locked(struct receiver_state *state, uint64_t now_ms) {
    size_t a_cap, a_occ, a_step, a_occ_pct, a_grow;
    size_t c_cap, c_occ, c_step, c_occ_pct, c_grow;
    uint8_t pressure_fast;
    int audio_grow_fail = 0;
    int cdg_grow_fail = 0;

    if (state == NULL) {
        return;
    }
    if (now_ms < s_rx_jb_adapt_last_tune_ms || (now_ms - s_rx_jb_adapt_last_tune_ms) < (uint64_t) DASHCDG_RX_JB_ADAPT_TUNE_MS) {
        return;
    }
    s_rx_jb_adapt_last_tune_ms = now_ms;
    if (s_rx_jb_adapt_last_resize_fail_ms != 0U && now_ms - s_rx_jb_adapt_last_resize_fail_ms < (uint64_t) DASHCDG_RX_JB_ADAPT_FAIL_COOLDOWN_MS) {
        return;
    }

    pressure_fast = 0U;
    if (state->cdg_batch_jitter.reordered_batches > s_rx_jb_adapt_last_cdg_reordered) {
        pressure_fast = 1U;
    }
    s_rx_jb_adapt_last_cdg_reordered = state->cdg_batch_jitter.reordered_batches;
    if (state->cdg_batch_jitter.pending_drops > s_rx_jb_adapt_last_cdg_pending_drops) {
        pressure_fast = 1U;
    }
    s_rx_jb_adapt_last_cdg_pending_drops = state->cdg_batch_jitter.pending_drops;
    if (state->audio_jitter.pending_drops > s_rx_jb_adapt_last_audio_pending_drops) {
        pressure_fast = 1U;
    }
    s_rx_jb_adapt_last_audio_pending_drops = state->audio_jitter.pending_drops;

    a_cap = dashcdg_audio_jitter_capacity(&state->audio_jitter);
    a_occ = dashcdg_audio_jitter_occupied_count(&state->audio_jitter);
    a_step = pressure_fast ? (size_t) DASHCDG_RX_JB_ADAPT_STEP_FAST_SLOTS : (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS;
    a_occ_pct = pressure_fast ? (size_t) DASHCDG_RX_JB_ADAPT_OCC_PCT_FAST : (size_t) DASHCDG_RX_JB_ADAPT_OCC_PCT;
    if (s_rx_jb_adapt_fail_streak >= (uint8_t) DASHCDG_RX_JB_ADAPT_FAIL_STREAK_STEP_DOWN && a_step > (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS) {
        a_step = (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS;
    }
    if (a_cap > 0U && (a_occ * 100U) >= ((size_t) DASHCDG_RX_JB_ADAPT_BURST_OCC_PCT * a_cap)) {
        s_rx_jb_adapt_last_burst_audio_ms = now_ms;
    }
    if (a_cap > 0U && a_cap < (size_t) DASHCDG_RX_AUDIO_JB_HARD_MAX_SLOTS && (a_occ * 100U) >= (a_occ_pct * a_cap)) {
        a_grow = a_cap + a_step;
        if (a_grow > (size_t) DASHCDG_RX_AUDIO_JB_HARD_MAX_SLOTS) {
            a_grow = (size_t) DASHCDG_RX_AUDIO_JB_HARD_MAX_SLOTS;
        }
        if (a_grow > a_cap) {
            if (dashcdg_audio_jitter_resize(&state->audio_jitter, a_grow)) {
                s_rx_jb_adapt_last_grow_ms = now_ms;
                s_rx_jb_adapt_fail_streak = 0U;
            } else {
                audio_grow_fail = 1;
            }
        }
    }
    a_cap = dashcdg_audio_jitter_capacity(&state->audio_jitter);
    a_occ = dashcdg_audio_jitter_occupied_count(&state->audio_jitter);
    if ((now_ms - s_rx_jb_adapt_last_grow_ms) >= (uint64_t) DASHCDG_RX_JB_ADAPT_SHRINK_HOLDOFF_MS &&
            (now_ms - s_rx_jb_adapt_last_burst_audio_ms) >= (uint64_t) DASHCDG_RX_JB_ADAPT_BURST_HOLD_MS &&
            a_cap > (size_t) DASHCDG_RX_AUDIO_JB_BALANCED_SLOTS && (a_occ * 100U) <= ((size_t) DASHCDG_RX_JB_ADAPT_SHRINK_OCC_PCT * a_cap)) {
        (void) dashcdg_audio_jitter_resize(&state->audio_jitter, DASHCDG_RX_AUDIO_JB_BALANCED_SLOTS);
    }

    c_cap = dashcdg_cdg_batch_jitter_capacity(&state->cdg_batch_jitter);
    c_occ = dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter);
    c_step = pressure_fast ? (size_t) DASHCDG_RX_JB_ADAPT_STEP_FAST_SLOTS : (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS;
    c_occ_pct = pressure_fast ? (size_t) DASHCDG_RX_JB_ADAPT_OCC_PCT_FAST : (size_t) DASHCDG_RX_JB_ADAPT_OCC_PCT;
    if (s_rx_jb_adapt_fail_streak >= (uint8_t) DASHCDG_RX_JB_ADAPT_FAIL_STREAK_STEP_DOWN && c_step > (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS) {
        c_step = (size_t) DASHCDG_RX_JB_ADAPT_STEP_SLOTS;
    }
    if (c_cap > 0U && (c_occ * 100U) >= ((size_t) DASHCDG_RX_JB_ADAPT_BURST_OCC_PCT * c_cap)) {
        s_rx_jb_adapt_last_burst_cdg_ms = now_ms;
    }
    if (c_cap > 0U && c_cap < (size_t) DASHCDG_RX_CDG_JB_HARD_MAX_SLOTS && (c_occ * 100U) >= (c_occ_pct * c_cap)) {
        c_grow = c_cap + c_step;
        if (c_grow > (size_t) DASHCDG_RX_CDG_JB_HARD_MAX_SLOTS) {
            c_grow = (size_t) DASHCDG_RX_CDG_JB_HARD_MAX_SLOTS;
        }
        if (c_grow > c_cap) {
            if (dashcdg_cdg_batch_jitter_resize(&state->cdg_batch_jitter, c_grow)) {
                s_rx_jb_adapt_last_grow_ms = now_ms;
                s_rx_jb_adapt_fail_streak = 0U;
            } else {
                cdg_grow_fail = 1;
            }
        }
    }
    c_cap = dashcdg_cdg_batch_jitter_capacity(&state->cdg_batch_jitter);
    c_occ = dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter);
    if ((now_ms - s_rx_jb_adapt_last_grow_ms) >= (uint64_t) DASHCDG_RX_JB_ADAPT_SHRINK_HOLDOFF_MS &&
            (now_ms - s_rx_jb_adapt_last_burst_cdg_ms) >= (uint64_t) DASHCDG_RX_JB_ADAPT_BURST_HOLD_MS &&
            c_cap > (size_t) DASHCDG_RX_CDG_JB_BALANCED_SLOTS && (c_occ * 100U) <= ((size_t) DASHCDG_RX_JB_ADAPT_SHRINK_OCC_PCT * c_cap)) {
        (void) dashcdg_cdg_batch_jitter_resize(&state->cdg_batch_jitter, DASHCDG_RX_CDG_JB_BALANCED_SLOTS);
    }
    if (audio_grow_fail || cdg_grow_fail) {
        s_rx_jb_adapt_last_resize_fail_ms = now_ms;
        if (s_rx_jb_adapt_fail_streak < 255U) {
            s_rx_jb_adapt_fail_streak++;
        }
    }
}
#else
static void dashcdg_rx_jitter_adapt_reset_session(void) {
}

/*
 * Heap adapt path is disabled when both DASHCDG_*_JITTER_HEAP_BACKED macros are not set together, but
 * audio/cdg jitter must still be initialized or core insert/drain sees cap==0 (heap path) or a
 * zeroed struct and never decodes (see dashcdg_audio_jitter_insert / audio_jitter_drain_step).
 */
static void dashcdg_rx_jitter_heap_init(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    dashcdg_audio_jitter_init(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_init(&state->cdg_batch_jitter);
}

static void dashcdg_rx_jitter_heap_shutdown(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    dashcdg_audio_jitter_release(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_release(&state->cdg_batch_jitter);
}

static void dashcdg_rx_jitter_shrink_to_balanced_on_reset(struct receiver_state *state) {
    (void) state;
}

static void dashcdg_rx_adapt_jitter_capacity_locked(struct receiver_state *state, uint64_t now_ms) {
    (void) state;
    (void) now_ms;
}
#endif

static struct receiver_state g_receiver;
static struct dashcdg_desktop_audio *g_audio;
#if DASHCDG_RX_HAVE_GLUT
static struct dashcdg_gl_renderer g_renderer;
static int g_rx_use_win_gdi;
static int g_rx_gl_display_active;
static uint64_t g_rx_gl_resize_pause_until_ms;
static int g_rx_gl_pending_resize_w;
static int g_rx_gl_pending_resize_h;
static uint64_t g_rx_gl_last_resize_cb_ms;
static uint64_t g_rx_gl_move_guard_until_ms;
static int g_rx_gl_move_guard_logged;
#endif
static struct dashcdg_opus_decoder g_opus_decoder;
static struct dashcdg_nb_ima_state g_nb_ima_decoder;
static void *g_amr_wb_decoder;
static void *g_amr_nb_decoder;
static void *g_evrc_decoder;
static void *g_qcelp_decoder;
static void *g_bt_sbc_decoder;

static void dashcdg_rx_amr_decoders_release(void) {
    if (g_evrc_decoder != NULL) {
        dashcdg_qcelp8k_decoder_destroy(g_evrc_decoder);
        g_evrc_decoder = NULL;
    }
    if (g_qcelp_decoder != NULL) {
        dashcdg_qcelp13k_decoder_destroy(g_qcelp_decoder);
        g_qcelp_decoder = NULL;
    }
    if (g_bt_sbc_decoder != NULL) {
        dashcdg_bt_sbc_decoder_destroy(g_bt_sbc_decoder);
        g_bt_sbc_decoder = NULL;
    }
    if (g_amr_wb_decoder != NULL) {
        dashcdg_amr_wb_decoder_destroy(g_amr_wb_decoder);
        g_amr_wb_decoder = NULL;
    }
    if (g_amr_nb_decoder != NULL) {
        dashcdg_amr_nb_decoder_destroy(g_amr_nb_decoder);
        g_amr_nb_decoder = NULL;
    }
}

static int dashcdg_rx_init_audio_decoder_for_codec(
        uint8_t codec_id,
        uint16_t sample_rate,
        uint8_t channels,
        uint8_t frame_ms
) {
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
        dashcdg_opus_decoder_init(&g_opus_decoder, sample_rate, channels, frame_ms);
        return 1;
#else
        (void) sample_rate;
        (void) channels;
        (void) frame_ms;
        fprintf(stderr, "[rx] sender uses Opus; this build has no Opus decoder (SBC-like only)\n");
        return 0;
#endif
    }

    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        dashcdg_amr_wb_decoder_create(&g_amr_wb_decoder);
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return g_amr_wb_decoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        dashcdg_amr_nb_decoder_create(&g_amr_nb_decoder);
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return g_amr_nb_decoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_QCELP8K) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_qcelp8k_decoder_create(&g_evrc_decoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_qcelp13k_decoder_create(&g_qcelp_decoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_bt_sbc_decoder_create(&g_bt_sbc_decoder);
    }

    dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
    return 1;
}

static const char *g_endpoint_address;
static struct in_addr g_endpoint_in_addr;
static int g_endpoint_is_multicast;
static int g_endpoint_is_broadcast;
static uint32_t g_rx_stats_interval_ms = DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS;
static uint32_t g_rx_av_sync_log_ms = 0U;
static uint64_t g_rx_last_av_sync_log_ms = 0U;
/* Default to DAC-follow render clock so displayed CDG tracks locally heard audio. */
static int g_rx_graphics_clock_sender = 0;
static int32_t g_rx_graphics_trim_ms = 0;
static dashcdg_socket_t g_rx_stats_sockfd = DASHCDG_INVALID_SOCKET;
static dashcdg_socket_t g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
static struct sockaddr_in g_rx_stats_dest;
/** Send v4_repair_nack here (dedicated multicast when session is multicast; else same as stats dest). */
static struct sockaddr_in g_rx_repair_nack_dest;
static int g_rx_stats_port = DASHCDG_DEFAULT_NETWORK_STATS_PORT;
static int g_rx_repair_port = 0;
#define DASHCDG_RX_DEFAULT_REPAIR_PORT 24686
static int g_headless = 0;
static int g_audio_stream_started = 0;
static int g_audio_start_inflight = 0;
/*
 * After operator-driven decode re-enable (D toggle), require full target preroll before starting
 * output again. The cold-join cap is useful for first startup, but resume-at-cap (~160 ms) can
 * relaunch too shallow and re-underrun repeatedly on some Win11/WASAPI hosts.
 */
static int g_rx_force_full_preroll_start = 0;
#if DASHCDG_HAVE_PORTAUDIO
static unsigned int g_rx_pa_stream_inactive_streak;
#endif
static uint64_t g_rx_audio_start_fail_log_ms = 0U;
static int g_hud_visible = 0;
static int g_audio_muted = 0;
static int g_audio_decode_disabled = 0;
static volatile int g_rx_shutdown_requested = 0;
static pthread_mutex_t g_render_mutex;
static struct dashcdg_rx_render_snapshot g_render_snapshot;
static uint64_t g_rx_last_render_snapshot_local_ms = 0U;
static FILE *g_rx_pcm_dump_file;
static size_t g_rx_pcm_dump_frames_written;
static size_t g_rx_pcm_dump_frame_limit;
static int g_rx_pcm_dump_init_attempted;
static uint32_t g_rx_receiver_instance_id = 0U;
static struct dashcdg_async_logger g_rx_logger;
static int g_rx_logger_enabled;
#define DASHCDG_RX_NACK_QUEUE_CAPACITY 256U
struct dashcdg_rx_nack_job {
    uint64_t now_ms;
    uint8_t stream_type;
    uint32_t group_id;
    uint16_t observed_group_size;
    uint16_t missing_mask;
};
static pthread_mutex_t g_rx_nack_queue_mutex;
static pthread_cond_t g_rx_nack_queue_cond;
static struct dashcdg_rx_nack_job g_rx_nack_queue[DASHCDG_RX_NACK_QUEUE_CAPACITY];
static size_t g_rx_nack_queue_head = 0U;
static size_t g_rx_nack_queue_tail = 0U;
static size_t g_rx_nack_queue_count = 0U;
static uint64_t g_rx_nack_queue_dropped = 0U;
static void dashcdg_rx_send_v4_repair_nack_locked(
        uint64_t now_ms,
        uint8_t stream_type,
        uint32_t group_id,
        uint16_t observed_group_size,
        uint16_t missing_mask
);

static uint32_t dashcdg_rx_render_snapshot_interval_ms(void) {
#if defined(DASHCDG_RX_UI_GDI_ONLY)
    return DASHCDG_RENDER_FRAME_INTERVAL_MS;
#elif defined(_WIN32) && DASHCDG_RX_HAVE_GLUT
    if (g_rx_use_win_gdi) {
        return DASHCDG_RENDER_FRAME_INTERVAL_MS;
    }
#endif
    return DASHCDG_RX_RENDER_SNAPSHOT_INTERVAL_MS;
}

static void dashcdg_rx_async_stdout_line(const char *line) {
    if (g_rx_logger_enabled && line != NULL) {
        dashcdg_async_logger_log_line(&g_rx_logger, DASHCDG_ASYNC_LOG_STDOUT, line);
    }
}

static void dashcdg_rx_sidecar_write_line(const char *line) {
    if (g_rx_logger_enabled && line != NULL) {
        dashcdg_async_logger_log_line(&g_rx_logger, DASHCDG_ASYNC_LOG_SIDECAR_ONLY, line);
    }
}

#define RX_OUT(...) \
    do { \
        char _dashcdg_rx_log_buf[DASHCDG_ASYNC_LOG_LINE_MAX]; \
        snprintf(_dashcdg_rx_log_buf, sizeof(_dashcdg_rx_log_buf), __VA_ARGS__); \
        _dashcdg_rx_log_buf[sizeof(_dashcdg_rx_log_buf) - 1U] = '\0'; \
        if (g_rx_logger_enabled) { \
            dashcdg_async_logger_log_line(&g_rx_logger, DASHCDG_ASYNC_LOG_STDOUT, _dashcdg_rx_log_buf); \
        } else { \
            (void) fputs(_dashcdg_rx_log_buf, stdout); \
            (void) fflush(stdout); \
        } \
    } while (0)

#define RX_ERR(...) \
    do { \
        char _dashcdg_rx_log_buf[DASHCDG_ASYNC_LOG_LINE_MAX]; \
        snprintf(_dashcdg_rx_log_buf, sizeof(_dashcdg_rx_log_buf), __VA_ARGS__); \
        _dashcdg_rx_log_buf[sizeof(_dashcdg_rx_log_buf) - 1U] = '\0'; \
        if (g_rx_logger_enabled) { \
            dashcdg_async_logger_log_line(&g_rx_logger, DASHCDG_ASYNC_LOG_STDERR, _dashcdg_rx_log_buf); \
        } else { \
            (void) fputs(_dashcdg_rx_log_buf, stderr); \
            (void) fflush(stderr); \
        } \
    } while (0)

static void dashcdg_rx_logger_shutdown_if_needed(void) {
    if (g_rx_logger_enabled) {
        dashcdg_async_logger_shutdown(&g_rx_logger);
        g_rx_logger_enabled = 0;
    }
}

static void dashcdg_rx_init_receiver_instance_id(void) {
    uint64_t seed = (uint64_t) dashcdg_clock_now_ms();

#ifdef _WIN32
    seed ^= ((uint64_t) (uint32_t) GetCurrentProcessId()) << 16;
#endif
    seed ^= (seed >> 33);
    seed *= 0xff51afd7ed558ccdULL;
    seed ^= (seed >> 33);
    g_rx_receiver_instance_id = (uint32_t) (seed ^ (seed >> 32));
    if (g_rx_receiver_instance_id == 0U) {
        g_rx_receiver_instance_id = 1U;
    }
}

static void dashcdg_rx_logger_boot(const char *argv0) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    char dir_path[MAX_PATH];
    char stem[128];
    char log_path[MAX_PATH * 2];
    char *base;
    char *dot;
    time_t now_t;
    struct tm now_tm;
    char line[sizeof(log_path) + 32U];
    DWORD tick_ms = GetTickCount();

    (void) argv0;
    if (GetModuleFileNameA(NULL, exe_path, (DWORD) sizeof(exe_path)) == 0 || exe_path[0] == '\0') {
        if (!dashcdg_async_logger_init(&g_rx_logger, NULL)) {
            return;
        }
        g_rx_logger_enabled = 1;
        return;
    }
    strncpy(dir_path, exe_path, sizeof(dir_path) - 1U);
    dir_path[sizeof(dir_path) - 1U] = '\0';
    base = strrchr(dir_path, '\\');
    if (base == NULL) {
        base = strrchr(dir_path, '/');
    }
    if (base == NULL) {
        if (!dashcdg_async_logger_init(&g_rx_logger, NULL)) {
            return;
        }
        g_rx_logger_enabled = 1;
        return;
    }
    *base++ = '\0';
    snprintf(stem, sizeof(stem), "%s", base);
    dot = strrchr(stem, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    now_t = time(NULL);
    if (localtime_s(&now_tm, &now_t) != 0) {
        memset(&now_tm, 0, sizeof(now_tm));
    }
    snprintf(
            log_path,
            sizeof(log_path),
            "%s\\%s-%04d%02d%02d-%02d%02d%02d-p%lu-t%lu.log",
            dir_path,
            stem,
            now_tm.tm_year + 1900,
            now_tm.tm_mon + 1,
            now_tm.tm_mday,
            now_tm.tm_hour,
            now_tm.tm_min,
            now_tm.tm_sec,
            (unsigned long) GetCurrentProcessId(),
            (unsigned long) tick_ms
    );
    if (!dashcdg_async_logger_init(&g_rx_logger, log_path)) {
        return;
    }
    g_rx_logger_enabled = 1;
    snprintf(line, sizeof(line), "[rx] sidecar log: %s", log_path);
    dashcdg_rx_async_stdout_line(line);
#else
    (void) argv0;
    if (!dashcdg_async_logger_init(&g_rx_logger, NULL)) {
        return;
    }
    g_rx_logger_enabled = 1;
#endif
}

static uint32_t dashcdg_rx_audio_host_latency_ms_locked(void);
static uint32_t dashcdg_rx_audio_target_total_latency_ms_locked(const struct receiver_state *state);
static uint32_t dashcdg_rx_audio_target_buffer_ms_locked(const struct receiver_state *state);
static uint16_t dashcdg_rx_startup_stage_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
);
static uint32_t dashcdg_rx_startup_flags_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
);
static uint32_t dashcdg_rx_stats_sanitized_audio_buffer_ms_locked(
        const struct receiver_state *state,
        uint32_t raw_buffered_ms
);
static uint16_t dashcdg_rx_stats_sanitized_startup_stage_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
);
static void dashcdg_rx_refresh_audio_latency_budget_locked(
        struct receiver_state *state,
        uint16_t playout_delay_ms,
        uint8_t frame_ms,
        uint32_t configured_ring_ms
);
static int32_t dashcdg_rx_audio_resample_trim_ppm_locked(struct receiver_state *state, uint32_t buffered_ms);
static int dashcdg_rx_audio_backpressure_hold_active_locked(const struct receiver_state *state, uint64_t local_now_ms);
static int dashcdg_rx_audio_recent_auto_recover_locked(const struct receiver_state *state, uint64_t local_now_ms);
static void dashcdg_rx_apply_equal_rate_trim_slip_locked(
        struct receiver_state *state,
        int32_t trim_ppm,
        int16_t *interleaved_pcm,
        size_t *inout_frames,
        unsigned int channels,
        size_t frame_capacity
);

static void dashcdg_rx_dump_pcm_to_file(const int16_t *pcm, size_t frame_count, unsigned int channels) {
    const char *dump_dir;
    char path[1024];
    size_t frames_to_write;

    if (pcm == NULL || frame_count == 0U || channels == 0U) {
        return;
    }

    if (!g_rx_pcm_dump_init_attempted) {
        const char *seconds_env;

        g_rx_pcm_dump_init_attempted = 1;
        dump_dir = getenv("DASHCDG_PCM_DUMP_DIR");
        seconds_env = getenv("DASHCDG_PCM_DUMP_SECONDS");
        g_rx_pcm_dump_frame_limit = DASHCDG_AUDIO_SAMPLE_RATE * 10U;
        if (seconds_env != NULL) {
            long seconds = strtol(seconds_env, NULL, 10);
            if (seconds > 0) {
                g_rx_pcm_dump_frame_limit = (size_t) seconds * DASHCDG_AUDIO_SAMPLE_RATE;
            }
        }
        if (dump_dir != NULL && dump_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/rx-stereo48-s16le.raw", dump_dir);
            g_rx_pcm_dump_file = fopen(path, "wb");
        }
    }

    if (g_rx_pcm_dump_file == NULL || g_rx_pcm_dump_frames_written >= g_rx_pcm_dump_frame_limit) {
        return;
    }

    frames_to_write = frame_count;
    if (frames_to_write > g_rx_pcm_dump_frame_limit - g_rx_pcm_dump_frames_written) {
        frames_to_write = g_rx_pcm_dump_frame_limit - g_rx_pcm_dump_frames_written;
    }
    if (frames_to_write == 0U) {
        return;
    }

    fwrite(pcm, sizeof(*pcm) * channels, frames_to_write, g_rx_pcm_dump_file);
    fflush(g_rx_pcm_dump_file);
    g_rx_pcm_dump_frames_written += frames_to_write;
}

static uint32_t dashcdg_rx_pcm_peak_abs_s16(const int16_t *pcm, size_t sample_count) {
    uint32_t peak = 0U;
    size_t i;

    if (pcm == NULL) {
        return 0U;
    }
    for (i = 0; i < sample_count; ++i) {
        int32_t v = (int32_t) pcm[i];
        uint32_t a = (uint32_t) (v < 0 ? -v : v);
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

static void dashcdg_rx_fill_rect(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        int width,
        int height,
        uint8_t color
) {
    int x0 = x < DASHCDG_VISIBLE_X ? DASHCDG_VISIBLE_X : x;
    int y0 = y < DASHCDG_VISIBLE_Y ? DASHCDG_VISIBLE_Y : y;
    int x1 = x + width;
    int y1 = y + height;

    if (state == NULL || width <= 0 || height <= 0) {
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
            state->framebuffer[DASHCDG_ARRAY_INDEX(col, row)] = color & 0x0FU;
        }
    }
}

static const uint8_t *dashcdg_rx_connect_font_rows(char c) {
    for (size_t i = 0; i < sizeof(g_dashcdg_connect_font) / sizeof(g_dashcdg_connect_font[0]); ++i) {
        if (g_dashcdg_connect_font[i].c == c) {
            return g_dashcdg_connect_font[i].rows;
        }
    }
    return NULL;
}

static void dashcdg_rx_draw_glyph(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        char c,
        int scale,
        uint8_t fg_color,
        uint8_t bg_color
) {
    const uint8_t *rows = dashcdg_rx_connect_font_rows(c);

    if (state == NULL || rows == NULL || scale <= 0) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            uint8_t color = ((rows[row] >> (4 - col)) & 0x01U) ? fg_color : bg_color;

            dashcdg_rx_fill_rect(
                    state,
                    x + (col * scale),
                    y + (row * scale),
                    scale,
                    scale,
                    color
            );
        }
    }
}

static void dashcdg_rx_draw_text(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        const char *text,
        int scale,
        uint8_t fg_color,
        uint8_t bg_color
) {
    if (state == NULL || text == NULL || scale <= 0) {
        return;
    }

    for (int i = 0; text[i] != '\0'; ++i) {
        dashcdg_rx_draw_glyph(state, x + (i * scale * 6), y, text[i], scale, fg_color, bg_color);
    }
}

static uint64_t dashcdg_rx_elapsed_ms_safe(uint64_t now_ms, uint64_t then_ms) {
    if (then_ms == 0U) {
        return 0U;
    }
    if (now_ms < then_ms) {
        /* QueryPerformanceCounter glitch or thread reordering: treat as "just now". */
        return 0U;
    }
    return now_ms - then_ms;
}

/*
 * Canvas is "ready" once we have applied live CDG/audio/video from the wire (not merely any UDP).
 * Without this, the first clock/session packet clears "no datagrams" and we paint black until the
 * first snapshot exists.
 *
 * v4 may deliver Opus frames before any CDG anchor/delta applies; hide the CONNECTING overlay once
 * media payloads exist so audio playback is not trapped behind a graphics-only gate.
 */
static int dashcdg_rx_stream_canvas_ready_locked(const struct receiver_state *state) {
    if (state == NULL) {
        return 0;
    }
    return state->live_packets_applied > 0U || state->v4_bridge_cdg_valid || state->cdg_snapshots_applied > 0U ||
            state->v4_audio_chunk_packets > 0U || state->v4_video_delta_packets > 0U;
}

static void dashcdg_rx_connecting_overlay_decide_locked(
        uint64_t local_now_ms,
        int *out_show,
        int *out_reconnecting
) {
    uint64_t since_dg;

    if (out_show == NULL || out_reconnecting == NULL) {
        return;
    }
    *out_reconnecting = 0;

    if (g_receiver.last_datagram_local_ms == 0U) {
        *out_show = 1;
        return;
    }

    since_dg = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_datagram_local_ms);
    if (since_dg >= DASHCDG_STREAM_LOSS_RECONNECT_MS) {
        *out_show = 1;
        *out_reconnecting = 1;
        return;
    }

    if (g_receiver.playback_paused) {
        *out_show = 0;
        return;
    }

    if (!dashcdg_rx_stream_canvas_ready_locked(&g_receiver)) {
        *out_show = 1;
        return;
    }

    *out_show = 0;
}

static void dashcdg_rx_render_connecting_state(
        struct dashcdg_cdg_state *state,
        uint64_t now_ms,
        int reconnecting
) {
    const char *title = reconnecting ? "RECONNECTING" : "CONNECTING";
    int pulse = (int) ((now_ms / 250U) % 4U);
    int stripe_phase = (int) ((now_ms / 400U) % 6U);
    int title_width = (int) strlen(title) * 24;
    int title_x = DASHCDG_VISIBLE_X + ((DASHCDG_VISIBLE_WIDTH - title_width) / 2);
    int title_y = DASHCDG_VISIBLE_Y + 54;

    if (state == NULL) {
        return;
    }

    dashcdg_cdg_state_init(state);
    state->color_table[0] = 0x05070F;
    state->color_table[1] = 0x101A34;
    state->color_table[2] = 0x204070;
    state->color_table[3] = 0x4F8BFF;
    state->color_table[4] = 0x63E6BE;
    state->color_table[5] = 0xFFE066;
    state->color_table[6] = 0xFF8FA3;
    state->color_table[7] = 0xFFFFFF;
    memset(state->transparency, 0, sizeof(state->transparency));

    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X,
            DASHCDG_VISIBLE_Y,
            DASHCDG_VISIBLE_WIDTH,
            DASHCDG_VISIBLE_HEIGHT,
            1
    );

    for (int stripe = 0; stripe < 6; ++stripe) {
        uint8_t color = (uint8_t) (((stripe + stripe_phase) % 3) == 0 ? 2 : (((stripe + stripe_phase) % 3) == 1 ? 3 : 4));

        dashcdg_rx_fill_rect(
                state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_Y + (stripe * 12),
                DASHCDG_VISIBLE_WIDTH,
                5,
                color
        );
        dashcdg_rx_fill_rect(
                state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_BOTTOM - ((stripe + 1) * 12),
                DASHCDG_VISIBLE_WIDTH,
                5,
                color
        );
    }

    dashcdg_rx_draw_text(state, title_x, title_y, title, 4, 7, 1);

    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 34,
            DASHCDG_VISIBLE_Y + 132,
            212,
            12,
            2
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 40 + (pulse * 48),
            DASHCDG_VISIBLE_Y + 128,
            28,
            20,
            reconnecting ? 6 : 5
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 54,
            DASHCDG_VISIBLE_Y + 166,
            184,
            8,
            3
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 54 + (pulse * 44),
            DASHCDG_VISIBLE_Y + 160,
            22,
            20,
            7
    );
}

static int dashcdg_rx_is_number(const char *value) {
    size_t length;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    length = strlen(value);
    for (size_t i = 0; i < length; ++i) {
        if (!isdigit((unsigned char) value[i])) {
            return 0;
        }
    }

    return 1;
}

static int dashcdg_rx_parse_ipv4_address(const char *value, struct in_addr *out_addr) {
    if (value == NULL || out_addr == NULL) {
        return 0;
    }

    return dashcdg_inet_pton(AF_INET, value, out_addr) == 1;
}

static void dashcdg_rx_format_multicast_interface(
        const struct dashcdg_multicast_interface *interface_info,
        char *buffer,
        size_t buffer_size
) {
    char address_buffer[DASHCDG_INET_ADDRSTRLEN];
    const char *interface_kind = "multicast";

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    buffer[0] = '\0';
    if (interface_info == NULL) {
        return;
    }

    if (interface_info->is_ethernet) {
        interface_kind = "ethernet";
    } else if (interface_info->is_wifi) {
        interface_kind = "wi-fi";
    } else if (interface_info->is_tailscale) {
        interface_kind = "tailscale";
    }

    if (dashcdg_inet_ntop(AF_INET, &interface_info->ipv4_addr, address_buffer, sizeof(address_buffer)) == NULL) {
        strncpy(address_buffer, "unknown", sizeof(address_buffer) - 1U);
        address_buffer[sizeof(address_buffer) - 1U] = '\0';
    }
    snprintf(buffer, buffer_size, "%s (%s %s)", interface_info->name, interface_kind, address_buffer);
}

static size_t dashcdg_rx_join_multicast_interfaces(
        dashcdg_socket_t sockfd,
        const struct in_addr *group_addr,
        const struct dashcdg_multicast_interface *interfaces,
        size_t interface_count
) {
    size_t joined = 0U;

    if (sockfd == DASHCDG_INVALID_SOCKET || group_addr == NULL) {
        return 0U;
    }

    if (interfaces != NULL && interface_count > 0U) {
        /*
         * Joining every enumerated interface can duplicate multicast payloads on hosts with
         * virtual adapters / bridge paths. Those delayed duplicates look like persistent audio
         * reorder storms (next_seq lags, pending stays near full). Prefer one interface only.
         */
        if (dashcdg_net_join_multicast_group(sockfd, group_addr, &interfaces[0].ipv4_addr)) {
            joined = 1U;
        }
    }
    if (joined == 0U && dashcdg_net_join_multicast_group(sockfd, group_addr, NULL)) {
        joined = 1U;
    }
    return joined;
}

static int dashcdg_rx_ipv4_is_multicast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order >= 0xE0000000U && host_order <= 0xEFFFFFFFU;
}

static int dashcdg_rx_ipv4_is_broadcast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    if (dashcdg_rx_ipv4_is_multicast(address)) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order == 0xFFFFFFFFU || (host_order & 0xFFU) == 0xFFU;
}

static void dashcdg_rx_print_usage(const char *argv0) {
#if DASHCDG_RX_HAVE_GLUT
    RX_ERR(
            "usage: %s [--help] [--headless] [--rx-drop-audio] [--rx-stats-ms <ms>] [--rx-stats-port <port>] [--rx-repair-port <port>] [--rx-av-sync-log-ms <ms>]\n"
            "       [--rx-graphics-clock dac|sender] [--rx-graphics-trim-ms <signed>] [--win-gdi|--gdi] [endpoint-address] [port]\n",
            argv0
    );
    RX_ERR( "  --win-gdi / --gdi   Windows only: force Win32 GDI instead of OpenGL\n");
    RX_ERR( "  default: OpenGL first; on Windows, falls back to GDI if GL init fails\n");
#else
    RX_ERR(
            "usage: %s [--help] [--headless] [--rx-drop-audio] [--rx-stats-ms <ms>] [--rx-stats-port <port>] [--rx-repair-port <port>] [--rx-av-sync-log-ms <ms>]\n"
            "       [--rx-graphics-clock dac|sender] [--rx-graphics-trim-ms <signed>] [endpoint-address] [port]\n",
            argv0
    );
#endif
    RX_ERR(
            "defaults: endpoint-address=%s port=%d (stats-port=%d)\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT,
            DASHCDG_DEFAULT_NETWORK_STATS_PORT
    );
    RX_ERR( "use --help or -h for receiver behaviour and v4 session notes.\n");
}

static void dashcdg_rx_cli_print_help(const char *argv0) {
    const char *prog = argv0 != NULL ? argv0 : "desktop-rx";

    RX_OUT( "%s — desktop receiver (v4 + v3)\n\n", prog);
#if DASHCDG_RX_HAVE_GLUT
    RX_OUT(
            "Synopsis: %s [--help] [--headless] [--rx-drop-audio] [--rx-stats-port <port>] [--rx-repair-port <port>] [--rx-av-sync-log-ms <ms>] [--rx-graphics-clock dac|sender] "
            "[--win-gdi|--gdi] [endpoint-address] [port]\n\n",
            prog
    );
    RX_OUT(
            "Listens for UDP multicast/broadcast on the given endpoint. Windowed mode shows CD+G; "
            "HUD is hidden by default (press I). M toggles mute; D toggles audio decode/drop; S prints a stats line.\n\n"
    );
#else
    RX_OUT( "Synopsis: %s [--help] [--headless] [--rx-stats-port <port>] [--rx-repair-port <port>] [endpoint-address] [port]\n\n", prog);
#endif
    RX_OUT(
            "V4 audio: decoders follow each v4_session_info packet. When the transmitter changes "
            "audio_codec_id (CLI --v4-audio-codec or the c hotkey on TX), the receiver tears down "
            "the old decoder, re-opens PortAudio if needed, and continues with the new codec.\n\n"
    );
    RX_OUT(
            "Network defaults: %s:%d (stats multicast port default: %d)\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT,
            DASHCDG_DEFAULT_NETWORK_STATS_PORT
    );
    RX_OUT(
            "\n--rx-stats-ms <ms>: v4 only; send periodic observability to the session multicast endpoint "
            "(default %u ms; 0 disables).\n",
            (unsigned) DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS
    );
    RX_OUT( "--rx-stats-port <port>: multicast stats/PTP peer port (default %d).\n", DASHCDG_DEFAULT_NETWORK_STATS_PORT);
    RX_OUT( "--rx-repair-port <port>: secondary repair stream port (default %d; set to media port to disable).\n", DASHCDG_RX_DEFAULT_REPAIR_PORT);
    RX_OUT(
            "\n--rx-av-sync-log-ms <ms>: stderr timeline line every N ms (0 = off) — dac vs sender vs snapshot.\n"
            "--rx-graphics-clock dac|sender: dac = align raster to locally heard audio (default); "
            "sender = network lyrics timeline.\n"
            "--rx-graphics-trim-ms <n>: add n ms to raster playback before seek (fine sync).\n"
            "--rx-drop-audio / --no-audio-decode: ignore audio packets and keep video/sync only.\n"
    );
}

static void receiver_state_reset(struct receiver_state *state) {
    uint64_t prior_live_packets;

    prior_live_packets = state->live_packets_applied;
    free(state->asset_bytes);
    free(state->chunk_seen);
    state->asset_bytes = NULL;
    state->chunk_seen = NULL;
    state->asset_size = 0;
    state->chunk_size = 0;
    state->chunk_count = 0;
    state->received_chunks = 0;
    state->session_start_ms = 0;
    state->v4_session_epoch_anchor_sender_ms = 0U;
    state->playback_base_ms = 0;
    state->playback_base_sender_ms = 0;
    state->last_audio_jitter_apply_local_ms = 0;
    state->last_audio_queue_success_local_ms = 0;
    state->last_audio_timestamp_advance_local_ms = 0;
    state->last_cdg_jitter_apply_local_ms = 0;
    state->audio_skip_hold_until_local_ms = 0;
    state->cdg_skip_hold_until_local_ms = 0;
    state->contiguous_prefix_chunks = 0;
    /* Datagram / parse counters are cumulative for the process (not reset per asset). */
    state->duplicate_chunks = 0;
    state->asset_bytes_written = 0;
    state->last_progress_local_ms = 0;
    state->rx_interarrival_jitter_ema_ms = 0U;
    state->rx_sender_skew_ema_inited = 0;
    state->rx_sender_skew_ema_ms = 0;
    state->rx_stats_report_seq = 0U;
    state->rx_stats_last_sent_local_ms = 0U;
    state->rx_stats_last_summary_local_ms = 0U;
    state->v4_rx_stats_peer_packets = 0U;
    state->audio_packets = 0;
    state->cdg_batch_packets = 0;
    state->ptp_sync_packets = 0;
    state->ptp_follow_up_packets = 0;
    state->ptp_delay_req_packets = 0;
    state->ptp_delay_resp_packets = 0;
    state->v4_session_info_packets = 0;
    state->v4_loading_screen_packets = 0;
    state->v4_video_anchor_packets = 0;
    state->v4_audio_chunk_packets = 0;
    state->v4_video_delta_packets = 0;
    state->v4_repair_window_packets = 0;
    state->v4_clock_sync_packets = 0;
    state->live_packets_applied = 0;
    state->audio_decode_failures = 0;
    state->audio_queue_overflows = 0;
    state->audio_missing_skips = 0;
    state->audio_hard_resync_events = 0;
    state->live_missing_skips = 0;
    state->fec_packets = 0;
    state->fec_audio_packets = 0;
    state->fec_cdg_packets = 0;
    state->cdg_snapshot_packets = 0;
    state->cdg_snapshots_applied = 0;
    state->fec_audio_recovered = 0;
    state->fec_cdg_recovered = 0;
    state->fec_recovery_failures = 0;
    state->cdg_unrecoverable_groups = 0;
    state->repair_nack_tx = 0;
    state->last_logged_audio_queue_overflows = 0;
    state->last_logged_audio_missing_skips = 0;
    state->last_logged_audio_hard_resync_events = 0;
    state->last_logged_live_missing_skips = 0;
    state->last_logged_audio_reordered_packets = 0;
    state->last_logged_cdg_reordered_batches = 0;
    state->last_logged_stream_underrun_events = g_audio != NULL ? g_audio->stream_underrun_events : 0;
    state->last_observed_stream_underrun_events = g_audio != NULL ? g_audio->stream_underrun_events : 0;
    state->last_observed_stream_underrun_frames = g_audio != NULL ? g_audio->stream_underrun_frames : 0;
    state->last_logged_host_underrun_fault_local_ms = 0;
    state->audio_arrival_gap_events = 0;
    state->audio_arrival_gap_max_ms = 0;
    state->audio_arrival_burst_events = 0;
    state->audio_arrival_burst_max_run = 0;
    state->audio_last_chunk_local_ms = 0;
    state->audio_burst_window_start_local_ms = 0;
    state->audio_burst_run_count = 0;
    state->last_audio_hard_resync_local_ms = 0;
    state->last_logged_audio_arrival_gap_events = 0;
    state->last_logged_audio_arrival_burst_events = 0;
    state->last_logged_audio_arrival_fault_local_ms = 0;
    state->sync_leader_instance_id_low16 = 0U;
    state->sync_leader_trim_bias_ppm = 0;
    state->sync_leader_last_update_local_ms = 0U;
    state->sync_group_mode = DASHCDG_TX_GROUP_SYNC_MODE_OFF;
    state->sync_group_target_latency_ms = 0U;
    state->sync_group_phase_spread_ms = 0U;
    state->announced_audio_sample_rate = 0;
    state->announced_audio_channels = 0;
    state->announced_playout_delay_ms = 0;
    state->announced_audio_frame_ms = 0;
    state->announced_transport_version = 0;
    state->announced_audio_profile_id = 0;
    state->announced_audio_codec_id = 0;
    state->announced_audio_fec_group_size = 0;
    state->announced_cdg_fec_group_size = 0;
    state->pending_sync_id = 0;
    state->pending_sync_rx_local_ms = 0;
    state->pending_sync_origin_remote_ms = 0;
    state->next_delay_request_id = 1;
    state->pending_delay_request_id = 0;
    state->pending_delay_request_local_ms = 0;
    state->sender_offset_ms = 0;
    state->sender_path_delay_ms = 0;
    state->sender_offset_step_ms = 0;
    state->sender_path_step_ms = 0;
    state->sender_offset_jitter_peak_ms = 0;
    state->sender_path_jitter_peak_ms = 0;
    state->sender_clock_updates = 0;
    state->ptp_exchange_successes = 0;
    state->ptp_fallback_updates = 0;
    state->last_clock_update_local_ms = 0;
    state->reader_ready = 0;
    state->have_clock = 0;
    state->playback_paused = 0;
    state->network_audio_enabled = 0;
    state->pending_sync_valid = 0;
    state->pending_delay_request_valid = 0;
    state->active_snapshot_id = 0;
    state->active_snapshot_packet_index = 0;
    state->active_snapshot_total_bytes = 0;
    state->active_snapshot_received_bytes = 0;
    state->active_snapshot_received_chunks = 0;
    memset(state->active_snapshot_bytes, 0, sizeof(state->active_snapshot_bytes));
    memset(state->active_snapshot_chunk_seen, 0, sizeof(state->active_snapshot_chunk_seen));
    state->active_v4_anchor_id = 0;
    state->active_v4_anchor_packet_index = 0;
    state->active_v4_anchor_total_bytes = 0;
    state->active_v4_anchor_received_bytes = 0;
    state->active_v4_anchor_received_chunks = 0;
    memset(state->active_v4_anchor_bytes, 0, sizeof(state->active_v4_anchor_bytes));
    memset(state->active_v4_anchor_chunk_seen, 0, sizeof(state->active_v4_anchor_chunk_seen));
    state->v4_loading_screen_kind = 0;
    state->v4_loading_screen_phase = 0;
    state->v4_loading_screen_active = 0;
    state->jitter_audio_decode_primed = 0;
    state->jitter_cdg_decode_primed = 0;
    dashcdg_audio_jitter_clear(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_clear(&state->cdg_batch_jitter);
    dashcdg_rx_jitter_adapt_reset_session();
    dashcdg_rx_jitter_shrink_to_balanced_on_reset(state);
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    memset(state->cdg_fec_groups, 0, sizeof(state->cdg_fec_groups));
    state->pcm_src_overlap_valid = 0;
    state->pcm_src_stream_in_samples = 0;
    state->pcm_src_stream_out_samples = 0;
#if defined(DASHCDG_HAVE_LIBSOXR)
    dashcdg_pcm_soxr_stream_reset();
#endif
    state->audio_target_total_latency_ms = 0U;
    state->audio_target_buffer_ms = 0U;
    state->audio_ring_capacity_ms = 0U;
    state->audio_host_output_latency_ms = 0U;
    state->audio_resample_trim_ppm = 0;
    state->audio_servo_enable_after_local_ms = 0U;
    state->audio_last_servo_update_local_ms = 0U;
    state->audio_last_queue_pressure_local_ms = 0U;
    state->audio_last_stall_recover_local_ms = 0U;
    state->last_session_change_local_ms = 0U;
    state->recovery_host_underrun_count = 0U;
    state->recovery_zero_buffer_count = 0U;
    state->recovery_silent_stall_count = 0U;
    state->source_idle_park_count = 0U;
    state->last_observed_stream_underrun_events = g_audio != NULL ? g_audio->stream_underrun_events : 0U;
    state->last_observed_stream_underrun_frames = g_audio != NULL ? g_audio->stream_underrun_frames : 0U;
    state->last_audio_timestamp_ms = -1;
    /*
     * Full reset must clear the last successful audio wire snapshot. If rx_audio_applied_valid stayed 1
     * across a session/track boundary, the next configure_audio_locked used warm skip-hold (~240 ms) and
     * need_audio_device_reconfigure could false-negative when wire params matched the *previous* track —
     * same symptoms as decode-drop re-enable (which clears this via reset_audio_decode_path).
     */
    state->rx_audio_applied_valid = 0;
    state->rx_audio_applied_wire_sr = 0U;
    state->rx_audio_applied_wire_ch = 0U;
    state->rx_audio_applied_frame_ms = 0U;
    state->rx_audio_applied_preroll_ms = 0U;
    state->rx_audio_applied_profile_id = 0U;
    state->rx_audio_applied_codec_id = 0U;
    memset(state->song_id, 0, sizeof(state->song_id));
    dashcdg_media_clock_init(&state->sender_clock);
    dashcdg_cdg_reader_free(&state->reader);
    dashcdg_cdg_reader_init(&state->reader);
    if (prior_live_packets > 0U) {
        state->v4_bridge_cdg = state->live_state;
        state->v4_bridge_cdg_valid = 1;
    } else {
        state->v4_bridge_cdg_valid = 0;
    }
    dashcdg_cdg_state_init(&state->live_state);
}

static void receiver_state_drop_asset_assembly_buffers(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    free(state->asset_bytes);
    state->asset_bytes = NULL;
    free(state->chunk_seen);
    state->chunk_seen = NULL;
    state->chunk_size = 0U;
    state->chunk_count = 0U;
    state->received_chunks = 0U;
    state->duplicate_chunks = 0U;
    state->contiguous_prefix_chunks = 0U;
    state->asset_bytes_written = 0U;
    state->reader_ready = 0;
    dashcdg_cdg_reader_free(&state->reader);
    dashcdg_cdg_reader_init(&state->reader);
}

static int receiver_state_prepare_asset(
        struct receiver_state *state,
        uint32_t asset_size,
        uint32_t chunk_size
) {
    size_t chunk_count;

    if (asset_size == 0 || chunk_size == 0) {
        return 0;
    }

    if (state->asset_bytes != NULL && state->asset_size == asset_size && state->chunk_size == chunk_size) {
        return 1;
    }

    receiver_state_reset(state);
    state->asset_bytes = (uint8_t *) calloc(asset_size, 1);
    if (state->asset_bytes == NULL) {
        return 0;
    }

    chunk_count = (asset_size + chunk_size - 1U) / chunk_size;
    state->chunk_seen = (uint8_t *) calloc(chunk_count, 1);
    if (state->chunk_seen == NULL) {
        free(state->asset_bytes);
        state->asset_bytes = NULL;
        return 0;
    }

    state->asset_size = asset_size;
    state->chunk_size = chunk_size;
    state->chunk_count = chunk_count;
    return 1;
}

static void receiver_state_try_finalize(struct receiver_state *state) {
    if (state->reader_ready || state->asset_size == 0 || state->received_chunks != state->chunk_count) {
        return;
    }

    if (!dashcdg_cdg_reader_load_memory(&state->reader, state->asset_bytes, state->asset_size)) {
        return;
    }

    if (!dashcdg_cdg_reader_build_keyframes(&state->reader)) {
        dashcdg_cdg_reader_free(&state->reader);
        dashcdg_cdg_reader_init(&state->reader);
        return;
    }

    state->reader_ready = 1;
    state->last_progress_local_ms = dashcdg_clock_now_ms();
    RX_OUT( "[rx] asset ready for %s\n", state->song_id[0] == '\0' ? "<unknown>" : state->song_id);
}

static uint64_t dashcdg_rx_deadline_after_ms(uint64_t local_now_ms, uint32_t delta_ms) {
    if (UINT64_MAX - local_now_ms < (uint64_t) delta_ms) {
        return UINT64_MAX;
    }
    return local_now_ms + (uint64_t) delta_ms;
}

static uint32_t dashcdg_rx_startup_skip_hold_ms(uint16_t playout_delay_ms, int warm_codec_handoff) {
    uint32_t hold_ms = (uint32_t) playout_delay_ms;

    /*
     * On fresh joins the first anchor often lands before the first contiguous live runway.
     * Allowing jitter SKIP during that bootstrap window turns ordinary packet transit into a
     * permanent hole. Hold skip decisions briefly so exact batches/frames can arrive.
     */
    if (hold_ms < 1500U) {
        hold_ms = 1500U;
    }
    /*
     * v4 codec/profile hot-swap while audio was already playing: a 1.5 s hold zeros primed_decode
     * for the jitter drain and pairs badly with brief TX encoder gaps — sounds like periodic 20 ms
     * dropouts until the hold expires. Keep a short gate only.
     */
    if (warm_codec_handoff != 0 && hold_ms > 240U) {
        hold_ms = 240U;
    }
    return hold_ms;
}

static uint32_t receiver_prefix_bytes_snapshot(const struct receiver_state *state) {
    uint32_t prefix_bytes = 0;

    if (state->asset_size > 0U && state->chunk_size > 0U) {
        if (state->contiguous_prefix_chunks >= state->chunk_count) {
            prefix_bytes = (uint32_t) state->asset_size;
        } else {
            prefix_bytes = (uint32_t) (state->contiguous_prefix_chunks * state->chunk_size);
            if (prefix_bytes > state->asset_size) {
                prefix_bytes = (uint32_t) state->asset_size;
            }
        }
    }

    return prefix_bytes;
}

static void receiver_state_refresh_prefix(struct receiver_state *state) {
    if (state->chunk_seen == NULL) {
        state->contiguous_prefix_chunks = 0;
        return;
    }

    while (state->contiguous_prefix_chunks < state->chunk_count &&
            state->chunk_seen[state->contiguous_prefix_chunks] != 0) {
        state->contiguous_prefix_chunks++;
    }
}

static uint32_t dashcdg_rx_read_u32(const uint8_t *src) {
    return ((uint32_t) src[0] << 24U) |
           ((uint32_t) src[1] << 16U) |
           ((uint32_t) src[2] << 8U) |
           (uint32_t) src[3];
}

static void dashcdg_rx_begin_snapshot_locked(
        struct receiver_state *state,
        uint32_t snapshot_id,
        uint64_t packet_index,
        uint32_t total_bytes
) {
    if (state == NULL) {
        return;
    }

    state->active_snapshot_id = snapshot_id;
    state->active_snapshot_packet_index = packet_index;
    state->active_snapshot_total_bytes = total_bytes;
    state->active_snapshot_received_bytes = 0;
    state->active_snapshot_received_chunks = 0;
    memset(state->active_snapshot_bytes, 0, sizeof(state->active_snapshot_bytes));
    memset(state->active_snapshot_chunk_seen, 0, sizeof(state->active_snapshot_chunk_seen));
}

static int dashcdg_rx_apply_snapshot_locked(struct receiver_state *state) {
    size_t offset = 0;
    uint64_t local_now_ms = 0U;

    if (state == NULL || state->active_snapshot_total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    if (!state->playback_paused &&
            state->cdg_snapshots_applied > 0 &&
            state->cdg_batch_jitter.initialized &&
            state->active_snapshot_packet_index < state->cdg_batch_jitter.next_packet_index) {
        /*
         * Never rewind the live CDG cursor after bootstrap has already established a
         * snapshot/anchor boundary for this session. TX anchors describe the sender's
         * current "next batch to send" boundary; once RX has a live epoch, applying an
         * older boundary recreates a permanently-missing gap and the skip/drop counters
         * explode.
         */
        return 0;
    }

    state->live_state.ts = state->active_snapshot_packet_index;
    state->live_state.display_h_offset = state->active_snapshot_bytes[offset++];
    state->live_state.display_v_offset = state->active_snapshot_bytes[offset++];
    memcpy(state->live_state.transparency, state->active_snapshot_bytes + offset, DASHCDG_COLORS);
    offset += DASHCDG_COLORS;
    for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
        state->live_state.color_table[i] = (int) dashcdg_rx_read_u32(state->active_snapshot_bytes + offset);
        offset += 4U;
    }
    memcpy(
            state->live_state.framebuffer,
            state->active_snapshot_bytes + offset,
            DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT
    );
    state->v4_bridge_cdg = state->live_state;
    state->v4_bridge_cdg_valid = 1;
    local_now_ms = dashcdg_clock_now_ms();

    /*
     * A snapshot/anchor replaces the live canvas at packet_index and establishes a new
     * post-snapshot CDG boundary. Do not treat pre-snapshot live deltas as proof that the
     * next delta after this anchor is already decode-primed; otherwise the late-skip gate
     * can race ahead before the first post-anchor batch arrives.
     */
    dashcdg_cdg_batch_jitter_apply_snapshot_seek(&state->cdg_batch_jitter, state->active_snapshot_packet_index);
    state->live_packets_applied = 0U;
    state->jitter_cdg_decode_primed = 1;
    state->last_cdg_jitter_apply_local_ms = local_now_ms;
    state->cdg_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 0)
    );
    state->cdg_snapshots_applied++;
    state->last_progress_local_ms = local_now_ms;
    return 1;
}

static int dashcdg_rx_load_active_snapshot_state_locked(struct receiver_state *state, struct dashcdg_cdg_state *out_state) {
    size_t offset = 0U;

    if (state == NULL || out_state == NULL || state->active_snapshot_total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->ts = state->active_snapshot_packet_index;
    out_state->display_h_offset = state->active_snapshot_bytes[offset++];
    out_state->display_v_offset = state->active_snapshot_bytes[offset++];
    memcpy(out_state->transparency, state->active_snapshot_bytes + offset, DASHCDG_COLORS);
    offset += DASHCDG_COLORS;
    for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
        out_state->color_table[i] = (int) dashcdg_rx_read_u32(state->active_snapshot_bytes + offset);
        offset += 4U;
    }
    memcpy(
            out_state->framebuffer,
            state->active_snapshot_bytes + offset,
            DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT
    );
    return 1;
}

static int dashcdg_rx_should_apply_v4_anchor_locked(const struct receiver_state *state, uint64_t anchor_packet_index) {
    uint64_t local_now_ms;
    uint64_t stall_ms;
    uint64_t repair_stall_threshold_ms;

    if (state == NULL) {
        return 0;
    }
    if (state->playback_paused) {
        return 1;
    }
    /*
     * First anchor/snapshot application must run even when no CDG batch has been inserted yet:
     * apply_snapshot_locked calls dashcdg_cdg_batch_jitter_apply_snapshot_seek, which initializes
     * the jitter cursor. Requiring jitter.initialized first deadlocked late joins (anchor before
     * deltas, or anchor completes before first batch insert).
     *
     * If live deltas already advanced the cursor past this anchor boundary, do not rewind.
     */
    if (state->cdg_snapshots_applied == 0U) {
        if (state->cdg_batch_jitter.initialized &&
                anchor_packet_index < state->cdg_batch_jitter.next_packet_index) {
            return 0;
        }
        return 1;
    }
    if (!state->cdg_batch_jitter.initialized) {
        return 0;
    }
    if (anchor_packet_index < state->cdg_batch_jitter.next_packet_index) {
        return 0;
    }

    /*
     * Periodic v4 anchors track the sender's current batch boundary, which is ahead of
     * local playout by the announced preroll. Applying every refresh in steady state
     * yanks the live cursor forward and makes subsequent in-order deltas look stale.
     * Treat later anchors as repair-only unless the live path has actually stalled.
     */
    local_now_ms = dashcdg_clock_now_ms();
    stall_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_progress_local_ms);
    repair_stall_threshold_ms = state->announced_playout_delay_ms > 0U ?
            (uint64_t) state->announced_playout_delay_ms * 2U : 1000U;
    if (repair_stall_threshold_ms < 1000U) {
        repair_stall_threshold_ms = 1000U;
    }

    return stall_ms >= repair_stall_threshold_ms ? 1 : 0;
}

static void dashcdg_rx_handle_snapshot_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;

    if (state == NULL || view == NULL || view->cdg_snapshot.snapshot_bytes == NULL) {
        return;
    }
    if (view->cdg_snapshot.total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES ||
            view->cdg_snapshot.chunk_length == 0 ||
            view->cdg_snapshot.snapshot_offset + view->cdg_snapshot.chunk_length > DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return;
    }

    if (state->active_snapshot_id != view->cdg_snapshot.snapshot_id ||
            state->active_snapshot_packet_index != view->cdg_snapshot.packet_index ||
            state->active_snapshot_total_bytes != view->cdg_snapshot.total_bytes) {
        dashcdg_rx_begin_snapshot_locked(
                state,
                view->cdg_snapshot.snapshot_id,
                view->cdg_snapshot.packet_index,
                view->cdg_snapshot.total_bytes
        );
    }

    chunk_index = view->cdg_snapshot.snapshot_offset / DASHCDG_MAX_CDG_SNAPSHOT_CHUNK;
    if (chunk_index >= DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT) {
        return;
    }
    if (state->active_snapshot_chunk_seen[chunk_index]) {
        return;
    }

    memcpy(
            state->active_snapshot_bytes + view->cdg_snapshot.snapshot_offset,
            view->cdg_snapshot.snapshot_bytes,
            view->cdg_snapshot.chunk_length
    );
    state->active_snapshot_chunk_seen[chunk_index] = 1;
    state->active_snapshot_received_chunks++;
    state->active_snapshot_received_bytes += view->cdg_snapshot.chunk_length;
    if (state->active_snapshot_received_bytes >= state->active_snapshot_total_bytes) {
        dashcdg_rx_apply_snapshot_locked(state);
    }
}

static void dashcdg_rx_begin_v4_anchor_locked(
        struct receiver_state *state,
        uint32_t anchor_id,
        uint64_t packet_index,
        uint32_t total_bytes
) {
    if (state == NULL) {
        return;
    }

    state->active_v4_anchor_id = anchor_id;
    state->active_v4_anchor_packet_index = packet_index;
    state->active_v4_anchor_total_bytes = total_bytes;
    state->active_v4_anchor_received_bytes = 0;
    state->active_v4_anchor_received_chunks = 0;
    memset(state->active_v4_anchor_bytes, 0, sizeof(state->active_v4_anchor_bytes));
    memset(state->active_v4_anchor_chunk_seen, 0, sizeof(state->active_v4_anchor_chunk_seen));
}

static int dashcdg_rx_decode_v4_anchor_locked(struct receiver_state *state) {
    struct dashcdg_cdg_state bridge_state;
    uint32_t expected_bytes;
    size_t src_offset = 4;
    size_t dst_offset = 0;

    if (state == NULL || state->active_v4_anchor_total_bytes < 4U) {
        return 0;
    }

    expected_bytes = dashcdg_rx_read_u32(state->active_v4_anchor_bytes);
    if (expected_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }

    while (src_offset + 1U < state->active_v4_anchor_total_bytes && dst_offset < expected_bytes) {
        uint8_t run_length = state->active_v4_anchor_bytes[src_offset++];
        uint8_t value = state->active_v4_anchor_bytes[src_offset++];
        if (run_length == 0) {
            return 0;
        }
        while (run_length-- > 0 && dst_offset < expected_bytes) {
            state->active_snapshot_bytes[dst_offset++] = value;
        }
    }

    if (dst_offset != expected_bytes) {
        return 0;
    }

    state->active_snapshot_id = state->active_v4_anchor_id;
    state->active_snapshot_packet_index = state->active_v4_anchor_packet_index;
    state->active_snapshot_total_bytes = expected_bytes;
    state->active_snapshot_received_bytes = expected_bytes;
    state->active_snapshot_received_chunks = DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT;
    memset(state->active_snapshot_chunk_seen, 1, sizeof(state->active_snapshot_chunk_seen));
    if (!dashcdg_rx_load_active_snapshot_state_locked(state, &bridge_state)) {
        return 0;
    }
    state->v4_bridge_cdg = bridge_state;
    state->v4_bridge_cdg_valid = 1;
    if (state->live_packets_applied > 0U && state->cdg_snapshots_applied == 0U && !state->reader_ready) {
        /*
         * Late joins can receive live deltas before the first v4 anchor arrives.
         * Those deltas may have already advanced live_packets_applied on top of a
         * blank canvas, which makes the renderer ignore the newly decoded bridge
         * forever. Re-arm the live path from the bridge so the next deltas apply
         * onto a valid picture instead of a permanently black one.
         */
        state->live_state = bridge_state;
        state->live_packets_applied = 0U;
    }
    if (!dashcdg_rx_should_apply_v4_anchor_locked(state, state->active_v4_anchor_packet_index)) {
        return 1;
    }
    return dashcdg_rx_apply_snapshot_locked(state);
}

static void dashcdg_rx_apply_loading_screen_locked(
        struct receiver_state *state,
        uint8_t kind,
        uint8_t phase
) {
    uint64_t now_ms = dashcdg_clock_now_ms();
    int reconnecting = 0;

    if (state == NULL) {
        return;
    }
    /*
     * TX may keep sending REPAIRING loading frames until first audio is on the wire
     * even when CDG batches are still sitting in the jitter buffer on slow CPUs.
     * Never repaint the loading bitmap over the live canvas once media packets exist.
     */
    if (state->v4_audio_chunk_packets > 0U || state->v4_video_delta_packets > 0U) {
        return;
    }
    if (state->live_packets_applied > 0U) {
        return;
    }
    if (state->cdg_snapshots_applied > 0 || state->cdg_batch_jitter.initialized) {
        return;
    }
    if (state->v4_bridge_cdg_valid) {
        return;
    }

    reconnecting = (kind == DASHCDG_V4_LOADING_SCREEN_REPAIRING || kind == DASHCDG_V4_LOADING_SCREEN_LATE_JOIN) ? 1 : 0;
    dashcdg_rx_render_connecting_state(&state->live_state, now_ms + ((uint64_t) phase * 125U), reconnecting);
    state->v4_loading_screen_kind = kind;
    state->v4_loading_screen_phase = phase;
    state->v4_loading_screen_active = 1;
}

static void dashcdg_rx_handle_v4_anchor_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;

    if (state == NULL || view == NULL || view->v4_video_anchor.anchor_bytes == NULL) {
        return;
    }
    if (view->v4_video_anchor.total_bytes == 0 ||
            view->v4_video_anchor.total_bytes > DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES ||
            view->v4_video_anchor.chunk_length == 0 ||
            view->v4_video_anchor.anchor_offset + view->v4_video_anchor.chunk_length >
                    view->v4_video_anchor.total_bytes) {
        return;
    }
    if (view->v4_video_anchor.anchor_format != DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS) {
        return;
    }

    if (state->active_v4_anchor_id != view->v4_video_anchor.anchor_id ||
            state->active_v4_anchor_packet_index != view->v4_video_anchor.packet_index ||
            state->active_v4_anchor_total_bytes != view->v4_video_anchor.total_bytes) {
        dashcdg_rx_begin_v4_anchor_locked(
                state,
                view->v4_video_anchor.anchor_id,
                view->v4_video_anchor.packet_index,
                view->v4_video_anchor.total_bytes
        );
    }

    chunk_index = view->v4_video_anchor.anchor_offset / DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE;
    if (chunk_index >= DASHCDG_V4_ANCHOR_CHUNK_COUNT || state->active_v4_anchor_chunk_seen[chunk_index]) {
        return;
    }

    memcpy(
            state->active_v4_anchor_bytes + view->v4_video_anchor.anchor_offset,
            view->v4_video_anchor.anchor_bytes,
            view->v4_video_anchor.chunk_length
    );
    state->active_v4_anchor_chunk_seen[chunk_index] = 1;
    state->active_v4_anchor_received_chunks++;
    state->active_v4_anchor_received_bytes += view->v4_video_anchor.chunk_length;
    if (state->active_v4_anchor_received_bytes >= state->active_v4_anchor_total_bytes &&
            dashcdg_rx_decode_v4_anchor_locked(state)) {
        state->v4_loading_screen_active = 0;
    }
}

static size_t dashcdg_rx_pending_cdg_count(const struct receiver_state *state) {
    if (state == NULL) {
        return 0;
    }

    return dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter);
}

static int64_t dashcdg_abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

static void dashcdg_rx_collect_fec_group_stats(
        const struct dashcdg_rx_fec_group groups[],
        size_t *tracked_groups,
        size_t *groups_with_parity,
        size_t *repairable_groups
) {
    size_t tracked = 0;
    size_t parity = 0;
    size_t repairable = 0;

    if (groups == NULL) {
        if (tracked_groups != NULL) {
            *tracked_groups = 0;
        }
        if (groups_with_parity != NULL) {
            *groups_with_parity = 0;
        }
        if (repairable_groups != NULL) {
            *repairable_groups = 0;
        }
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        size_t missing = 0;

        if (!groups[i].occupied) {
            continue;
        }
        tracked++;
        if ((groups[i].parity_present == 0 && groups[i].parity_present_mask == 0U) || groups[i].expected_group_size <= 1) {
            continue;
        }
        parity++;
        for (uint8_t j = 0; j < groups[i].expected_group_size; ++j) {
            if (!groups[i].member_present[j]) {
                missing++;
            }
        }
        if (missing == 1) {
            repairable++;
        }
    }

    if (tracked_groups != NULL) {
        *tracked_groups = tracked;
    }
    if (groups_with_parity != NULL) {
        *groups_with_parity = parity;
    }
    if (repairable_groups != NULL) {
        *repairable_groups = repairable;
    }
}

static void dashcdg_rx_note_clock_update_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        int from_ptp_exchange
) {
    int64_t offset_step;
    int64_t path_step;

    if (state == NULL) {
        return;
    }

    offset_step = dashcdg_abs_i64(state->sender_clock.offset_ms - state->sender_offset_ms);
    path_step = dashcdg_abs_i64(state->sender_clock.path_delay_ms - state->sender_path_delay_ms);
    state->sender_offset_step_ms = offset_step;
    state->sender_path_step_ms = path_step;
    if (offset_step > state->sender_offset_jitter_peak_ms) {
        state->sender_offset_jitter_peak_ms = offset_step;
    }
    if (path_step > state->sender_path_jitter_peak_ms) {
        state->sender_path_jitter_peak_ms = path_step;
    }
    state->sender_offset_ms = state->sender_clock.offset_ms;
    state->sender_path_delay_ms = state->sender_clock.path_delay_ms;
    state->sender_clock_updates++;
    state->last_clock_update_local_ms = local_now_ms;
    state->have_clock = 1;
    if (from_ptp_exchange) {
        state->ptp_exchange_successes++;
    } else {
        state->ptp_fallback_updates++;
    }
}

static void dashcdg_rx_format_audio_gate_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        char *buffer,
        size_t buffer_size
) {
    uint32_t buffered_ms;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    if (!state->network_audio_enabled) {
        snprintf(buffer, buffer_size, "net-audio-off");
    } else if (g_audio_decode_disabled) {
        snprintf(buffer, buffer_size, "audio-drop");
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 && !state->audio_jitter.initialized) {
        snprintf(buffer, buffer_size, "wait-first-audio");
    } else if (!state->have_clock) {
        snprintf(buffer, buffer_size, "wait-ptp");
    } else if (state->playback_paused) {
        snprintf(buffer, buffer_size, "paused");
    } else if (dashcdg_rx_audio_backpressure_hold_active_locked(state, local_now_ms)) {
        snprintf(buffer, buffer_size, "backpressure-hold");
    } else if (dashcdg_rx_audio_recent_auto_recover_locked(state, local_now_ms)) {
        snprintf(buffer, buffer_size, "auto-recover");
    } else if (g_audio == NULL) {
        snprintf(buffer, buffer_size, "wait-audio-init");
    } else if (g_audio_stream_started) {
        /*
         * After claim_audio_start, the PCM ring often dips below playout_delay/2 while the host
         * buffer drains — that is steady state, not startup preroll. Only !started uses the
         * half-preroll threshold (matches claim_audio_start_locked).
         * PortAudio/waveOut open runs asynchronously; until the device callback exists we are not
         * truly "running" from the user's perspective.
         */
        if (g_audio != NULL && !dashcdg_desktop_audio_output_device_ready(g_audio)) {
            snprintf(buffer, buffer_size, "starting-dac");
        } else {
            snprintf(buffer, buffer_size, "running");
        }
    } else if (buffered_ms < dashcdg_rx_audio_target_buffer_ms_locked(state)) {
        snprintf(buffer, buffer_size, "wait-preroll %u/%u", (unsigned int) buffered_ms,
                (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(state));
    } else {
        snprintf(buffer, buffer_size, "ready-to-start");
    }
}

static void dashcdg_rx_format_render_gate_locked(
        const struct receiver_state *state,
        char *buffer,
        size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (state->playback_paused && state->cdg_snapshots_applied > 0) {
        snprintf(buffer, buffer_size, "pause-screen");
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 &&
            state->v4_loading_screen_active && state->cdg_snapshots_applied == 0) {
        snprintf(buffer, buffer_size, "loading-screen");
    } else if (state->live_packets_applied > 0U) {
        snprintf(buffer, buffer_size, "live");
    } else if (state->v4_bridge_cdg_valid) {
        snprintf(buffer, buffer_size, "anchor-ready");
    } else if (state->reader_ready) {
        snprintf(buffer, buffer_size, "asset-cache-ready");
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 &&
            state->asset_size > 0U && state->chunk_count == 0U) {
        /*
         * v4 does not assemble the .cdg file on the wire (chunk_count stays 0). This state means
         * we have session metadata but not yet anchor+deltas on the live canvas — not “asset chunks”.
         */
        snprintf(buffer, buffer_size, "v4-startup");
    } else if (state->asset_size == 0U && state->chunk_count == 0U) {
        snprintf(buffer, buffer_size, "wait-announce");
    } else if (state->cdg_snapshots_applied > 0) {
        snprintf(
                buffer,
                buffer_size,
                "live-snapshot %u/%u",
                (unsigned int) state->received_chunks,
                (unsigned int) state->chunk_count);
    } else {
        snprintf(
                buffer,
                buffer_size,
                "wait-bootstrap %u/%u",
                (unsigned int) state->received_chunks,
                (unsigned int) state->chunk_count);
    }
}

static int dashcdg_rx_sender_playback_now_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint64_t *out_playback_ms
) {
    int64_t sender_now_ms;
    uint64_t playback_ms;

    if (state == NULL || out_playback_ms == NULL || !state->have_clock || state->playback_base_sender_ms == 0U) {
        return 0;
    }

    sender_now_ms = dashcdg_media_clock_remote_now(&state->sender_clock, (int64_t) local_now_ms);
    playback_ms = state->playback_base_ms;
    if (!state->playback_paused && sender_now_ms > (int64_t) state->playback_base_sender_ms) {
        playback_ms += (uint64_t) (sender_now_ms - (int64_t) state->playback_base_sender_ms);
    }

    *out_playback_ms = playback_ms;
    return 1;
}

static int dashcdg_rx_local_audio_playback_now_locked(uint64_t *out_playback_ms) {
    int audio_ts;

    if (out_playback_ms == NULL || g_audio == NULL || !g_audio_stream_started) {
        return 0;
    }

    audio_ts = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    if (audio_ts < 0) {
        return 0;
    }

    *out_playback_ms = (uint64_t) audio_ts;
    return 1;
}

/*
 * CDG frame selection for both GL and Win32 GDI uses the same render snapshot.
 * Default to DAC timing so raster follows locally heard audio. Sender/network
 * timing remains available via --rx-graphics-clock sender and is held back by
 * estimated local output pipeline delay to avoid outrunning heard audio.
 */
static int dashcdg_rx_u64_playback_ms_to_int_safe(uint64_t ms) {
    if (ms >= (uint64_t) INT_MAX) {
        return INT_MAX;
    }
    return (int) ms;
}

static int dashcdg_rx_is_stale_prior_session_media_locked(
        const struct receiver_state *state,
        const struct dashcdg_packet_view *view
);

static uint32_t dashcdg_rx_audio_host_latency_ms_locked(void) {
    uint32_t host_ms = 0U;

    if (g_audio != NULL) {
        host_ms = dashcdg_desktop_audio_output_latency_ms(g_audio);
    }
    if (host_ms == 0U) {
        host_ms = DASHCDG_RX_OUTPUT_LATENCY_FALLBACK_MS;
    }
    return host_ms;
}

static uint32_t dashcdg_rx_audio_target_total_latency_ms_locked(const struct receiver_state *state) {
    if (state != NULL && state->audio_target_total_latency_ms > 0U) {
        return state->audio_target_total_latency_ms;
    }
    if (state != NULL && state->announced_playout_delay_ms > 0U) {
        return (uint32_t) state->announced_playout_delay_ms;
    }
    return DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
}

static uint32_t dashcdg_rx_audio_target_buffer_ms_locked(const struct receiver_state *state) {
    uint32_t fallback;

    if (state != NULL && state->audio_target_buffer_ms > 0U) {
        return state->audio_target_buffer_ms;
    }
    if (state != NULL && state->announced_playout_delay_ms > 0U) {
        fallback = (uint32_t) state->announced_playout_delay_ms / 2U;
    } else {
        fallback = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (fallback < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        fallback = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    return fallback;
}

static uint32_t dashcdg_rx_stats_sanitized_audio_buffer_ms_locked(
        const struct receiver_state *state,
        uint32_t raw_buffered_ms
) {
    uint32_t max_reasonable_ms = DASHCDG_RX_MAX_RING_CAPACITY_MS;

    if (state != NULL && state->audio_ring_capacity_ms > 0U && state->audio_ring_capacity_ms > max_reasonable_ms) {
        max_reasonable_ms = state->audio_ring_capacity_ms;
    }
    max_reasonable_ms += DASHCDG_RX_APP_RING_HEADROOM_MS;
    if (raw_buffered_ms > max_reasonable_ms) {
        /*
         * Never report 0 here: TX treats audio_buffer_ms==0 as empty-buf (hard receiver underrun).
         * A bogus high raw read (rate mismatch, ring repair glitch) must clamp, not masquerade as empty.
         */
        return max_reasonable_ms;
    }
    return raw_buffered_ms;
}

static uint16_t dashcdg_rx_stats_sanitized_startup_stage_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
) {
    uint16_t stage = dashcdg_rx_startup_stage_locked(state, local_now_ms, buffered_ms);

    if (stage > DASHCDG_V4_RX_STARTUP_RECOVERING) {
        stage = DASHCDG_V4_RX_STARTUP_UNKNOWN;
    }
    return stage;
}

static uint32_t dashcdg_rx_audio_servo_warmup_ms_locked(const struct receiver_state *state) {
    if (state != NULL && state->sync_group_mode == DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE) {
        return DASHCDG_RX_QUEUE_SERVO_WARMUP_SYNC_ACTIVE_MS;
    }
    return DASHCDG_RX_QUEUE_SERVO_WARMUP_MS;
}

static void dashcdg_rx_refresh_audio_latency_budget_locked(
        struct receiver_state *state,
        uint16_t playout_delay_ms,
        uint8_t frame_ms,
        uint32_t configured_ring_ms
) {
    uint32_t total_target_ms;
    uint32_t host_ms;
    uint32_t min_target_ms;
    uint32_t target_buffer_ms;
    uint32_t ring_capacity_ms;

    if (state == NULL) {
        return;
    }

    total_target_ms = playout_delay_ms > 0U ? (uint32_t) playout_delay_ms : DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
    host_ms = dashcdg_rx_audio_host_latency_ms_locked();
    min_target_ms = frame_ms > 0U ? (uint32_t) frame_ms * 4U : DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    if (min_target_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        min_target_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }

    if (total_target_ms > host_ms + DASHCDG_RX_APP_RING_SAFETY_MS) {
        target_buffer_ms = total_target_ms - host_ms - DASHCDG_RX_APP_RING_SAFETY_MS;
    } else {
        target_buffer_ms = min_target_ms;
    }
    if (target_buffer_ms < min_target_ms) {
        target_buffer_ms = min_target_ms;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_APP_RING_TARGET_MS;
    }

    ring_capacity_ms = configured_ring_ms;
    if (ring_capacity_ms == 0U) {
        ring_capacity_ms = target_buffer_ms + DASHCDG_RX_APP_RING_HEADROOM_MS;
        if (ring_capacity_ms < DASHCDG_RX_MIN_RING_CAPACITY_MS) {
            ring_capacity_ms = DASHCDG_RX_MIN_RING_CAPACITY_MS;
        }
        if (ring_capacity_ms > DASHCDG_RX_MAX_RING_CAPACITY_MS) {
            ring_capacity_ms = DASHCDG_RX_MAX_RING_CAPACITY_MS;
        }
    }
    if (ring_capacity_ms < target_buffer_ms) {
        ring_capacity_ms = target_buffer_ms;
    }

    state->audio_target_total_latency_ms = total_target_ms;
    state->audio_target_buffer_ms = target_buffer_ms;
    state->audio_ring_capacity_ms = ring_capacity_ms;
    state->audio_host_output_latency_ms = host_ms;
}

static int32_t dashcdg_rx_audio_resample_trim_ppm_locked(struct receiver_state *state, uint32_t buffered_ms) {
    int32_t error_ms;
    int32_t abs_error_ms;
    int32_t desired_ppm;
    int32_t leader_bias_ppm = 0;
    uint32_t target_buffer_ms;
    int32_t delta_ppm;
    int32_t step_ppm;
    uint64_t now_ms;

    if (state == NULL) {
        return 0;
    }
    if (g_audio == NULL || !g_audio_stream_started || !dashcdg_desktop_audio_output_device_ready(g_audio)) {
        state->audio_resample_trim_ppm -= state->audio_resample_trim_ppm / 4;
        return state->audio_resample_trim_ppm;
    }
    now_ms = dashcdg_clock_now_ms();
    state->audio_last_servo_update_local_ms = now_ms;
    if ((state->audio_servo_enable_after_local_ms != 0U && now_ms < state->audio_servo_enable_after_local_ms) ||
            (state->audio_last_queue_pressure_local_ms != 0U &&
                    now_ms - state->audio_last_queue_pressure_local_ms < DASHCDG_RX_QUEUE_SERVO_BACKPRESSURE_HOLD_MS)) {
        state->audio_resample_trim_ppm -= state->audio_resample_trim_ppm / 4;
        return state->audio_resample_trim_ppm;
    }

    target_buffer_ms = dashcdg_rx_audio_target_buffer_ms_locked(state);
    if (state->sync_group_mode == DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE &&
            state->sync_group_target_latency_ms > 0U &&
            state->audio_host_output_latency_ms > 0U) {
        uint32_t active_target_ms = state->sync_group_target_latency_ms;
        if (active_target_ms > state->audio_host_output_latency_ms) {
            active_target_ms -= state->audio_host_output_latency_ms;
        }
        if (active_target_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
            active_target_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
        }
        if (state->audio_ring_capacity_ms > 0U && active_target_ms > state->audio_ring_capacity_ms) {
            active_target_ms = state->audio_ring_capacity_ms;
        }
        target_buffer_ms = active_target_ms;
    }
    error_ms = (int32_t) target_buffer_ms - (int32_t) buffered_ms;
    if (error_ms > -DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS && error_ms < DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS) {
        desired_ppm = 0;
    } else {
        int32_t gain_ppm_per_ms = DASHCDG_RX_QUEUE_SERVO_GAIN_PPM_PER_MS;
        if (error_ms > -DASHCDG_RX_QUEUE_SERVO_FINE_BAND_MS && error_ms < DASHCDG_RX_QUEUE_SERVO_FINE_BAND_MS) {
            gain_ppm_per_ms = DASHCDG_RX_QUEUE_SERVO_FINE_GAIN_PPM_PER_MS;
        }
        desired_ppm = error_ms * gain_ppm_per_ms;
        if (desired_ppm > DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        } else if (desired_ppm < -DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = -DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        }
    }
    if (state->sync_group_mode == DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE &&
            state->sync_leader_last_update_local_ms != 0U &&
            now_ms >= state->sync_leader_last_update_local_ms &&
            now_ms - state->sync_leader_last_update_local_ms <= DASHCDG_RX_SYNC_LEADER_BIAS_STALE_MS &&
            (state->last_session_change_local_ms == 0U ||
                    now_ms - state->last_session_change_local_ms >= DASHCDG_RX_LEADER_BIAS_HOLDOFF_AFTER_TRACK_MS) &&
            state->sync_leader_trim_bias_ppm != 0 &&
            state->sync_leader_instance_id_low16 != 0U &&
            (uint16_t) (g_rx_receiver_instance_id & 0xffffU) != state->sync_leader_instance_id_low16) {
        leader_bias_ppm = (int32_t) state->sync_leader_trim_bias_ppm;
        if (leader_bias_ppm > DASHCDG_RX_SYNC_LEADER_BIAS_PPM_MAX) {
            leader_bias_ppm = DASHCDG_RX_SYNC_LEADER_BIAS_PPM_MAX;
        } else if (leader_bias_ppm < -DASHCDG_RX_SYNC_LEADER_BIAS_PPM_MAX) {
            leader_bias_ppm = -DASHCDG_RX_SYNC_LEADER_BIAS_PPM_MAX;
        }
        desired_ppm += leader_bias_ppm;
        if (desired_ppm > DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        } else if (desired_ppm < -DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = -DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        }
    }

    delta_ppm = desired_ppm - state->audio_resample_trim_ppm;
    abs_error_ms = error_ms < 0 ? -error_ms : error_ms;
    if (abs_error_ms >= 40) {
        step_ppm = delta_ppm / 2;
    } else if (abs_error_ms >= 20) {
        step_ppm = delta_ppm / 3;
    } else if (abs_error_ms >= 10) {
        step_ppm = delta_ppm / 4;
    } else {
        step_ppm = delta_ppm / 6;
    }
    if (step_ppm == 0 && delta_ppm != 0) {
        step_ppm = delta_ppm > 0 ? 1 : -1;
    }
    state->audio_resample_trim_ppm += step_ppm;
    return state->audio_resample_trim_ppm;
}

static int dashcdg_rx_audio_backpressure_hold_active_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint32_t buffered_ms;
    uint32_t target_ms;
    uint32_t release_threshold_ms;

    if (state == NULL || state->audio_last_queue_pressure_local_ms == 0U ||
            local_now_ms - state->audio_last_queue_pressure_local_ms >= DASHCDG_RX_QUEUE_SERVO_BACKPRESSURE_HOLD_MS ||
            g_audio == NULL || !g_audio_stream_started) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    target_ms = dashcdg_rx_audio_target_buffer_ms_locked(state);
    release_threshold_ms = target_ms;
    if (state->announced_audio_frame_ms > 0U) {
        release_threshold_ms += (uint32_t) state->announced_audio_frame_ms;
    }

    return buffered_ms >= release_threshold_ms;
}

static int dashcdg_rx_audio_recent_auto_recover_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    if (state == NULL || state->audio_last_stall_recover_local_ms == 0U) {
        return 0;
    }
    return local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS;
}

static int dashcdg_rx_source_idle_and_drained_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint64_t since_last_dg_ms;
    uint64_t since_last_queue_success_ms;
    uint64_t since_last_ts_advance_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled || state->playback_paused) {
        return 0;
    }
    if (state->last_datagram_local_ms == 0U) {
        return 0;
    }

    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_datagram_local_ms);
    if (since_last_dg_ms < DASHCDG_RX_SOURCE_IDLE_PARK_MS) {
        return 0;
    }
    /*
     * Do not park output immediately after startup/recovery: RX can briefly drain app-ring while
     * sender clock catches up, and aggressive parking here causes the observed ~1-2s audio drop.
     */
    since_last_queue_success_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_queue_success_local_ms);
    since_last_ts_advance_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_timestamp_advance_local_ms);
    if (since_last_queue_success_ms < DASHCDG_RX_SOURCE_IDLE_PARK_MS ||
            since_last_ts_advance_ms < DASHCDG_RX_SOURCE_IDLE_PARK_MS) {
        return 0;
    }
    if (dashcdg_audio_jitter_occupied_count(&state->audio_jitter) != 0U) {
        return 0;
    }
    if (dashcdg_rx_pending_cdg_count(state) != 0U) {
        return 0;
    }
    if (g_audio != NULL && dashcdg_desktop_audio_buffered_ms(g_audio) != 0U) {
        return 0;
    }

    return 1;
}static uint16_t dashcdg_rx_startup_stage_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
) {
    if (state == NULL) {
        return DASHCDG_V4_RX_STARTUP_UNKNOWN;
    }
    if (state->playback_paused && state->cdg_snapshots_applied > 0U) {
        return DASHCDG_V4_RX_STARTUP_PAUSED;
    }
    if (dashcdg_rx_source_idle_and_drained_locked(state, local_now_ms)) {
        return DASHCDG_V4_RX_STARTUP_SOURCE_IDLE;
    }
    if (dashcdg_rx_audio_recent_auto_recover_locked(state, local_now_ms)) {
        return DASHCDG_V4_RX_STARTUP_RECOVERING;
    }
    if (state->live_packets_applied > 0U) {
        return DASHCDG_V4_RX_STARTUP_RUNNING;
    }
    if (g_audio != NULL && !dashcdg_desktop_audio_output_device_ready(g_audio)) {
        return DASHCDG_V4_RX_STARTUP_WAIT_PREROLL;
    }
    if (buffered_ms < dashcdg_rx_audio_target_buffer_ms_locked(state)) {
        return DASHCDG_V4_RX_STARTUP_WAIT_PREROLL;
    }
    if (state->v4_loading_screen_active && state->cdg_snapshots_applied == 0U) {
        return DASHCDG_V4_RX_STARTUP_LOADING_SCREEN;
    }
    if (state->v4_bridge_cdg_valid) {
        return DASHCDG_V4_RX_STARTUP_ANCHOR_READY;
    }
    if (state->reader_ready) {
        return DASHCDG_V4_RX_STARTUP_ASSET_READY;
    }
    if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 &&
            state->asset_size > 0U && state->chunk_count == 0U) {
        return DASHCDG_V4_RX_STARTUP_V4_METADATA;
    }
    if (state->asset_size == 0U && state->chunk_count == 0U) {
        return DASHCDG_V4_RX_STARTUP_WAIT_ANNOUNCE;
    }
    if (buffered_ms >= dashcdg_rx_audio_target_buffer_ms_locked(state)) {
        return DASHCDG_V4_RX_STARTUP_READY_TO_START;
    }
    return DASHCDG_V4_RX_STARTUP_UNKNOWN;
}

static uint32_t dashcdg_rx_startup_flags_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint32_t buffered_ms
) {
    uint32_t flags = 0U;

    if (state == NULL) {
        return 0U;
    }
    if (state->have_clock) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_HAVE_CLOCK;
    }
    if (state->network_audio_enabled) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_NETWORK_AUDIO_ENABLED;
    }
    if (g_audio_stream_started) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_AUDIO_STREAM_STARTED;
    }
    if ((g_audio != NULL ? dashcdg_desktop_audio_is_muted(g_audio) : g_audio_muted) != 0) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_MUTED;
    }
    if (state->v4_loading_screen_active) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_LOADING_SCREEN_ACTIVE;
    }
    if (state->v4_bridge_cdg_valid) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_BRIDGE_READY;
    }
    if (state->live_packets_applied > 0U) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_LIVE_ACTIVE;
    }
    if (dashcdg_rx_audio_recent_auto_recover_locked(state, local_now_ms)) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_RECOVERY_COOLDOWN;
    }
    if (dashcdg_rx_source_idle_and_drained_locked(state, local_now_ms)) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_SOURCE_IDLE;
    }
    if (dashcdg_rx_audio_backpressure_hold_active_locked(state, local_now_ms)) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_QUEUE_PRESSURE;
    }
    if (buffered_ms >= dashcdg_rx_audio_target_buffer_ms_locked(state)) {
        flags |= DASHCDG_V4_RX_STARTUP_FLAG_AUDIO_PREROLL_READY;
    }
    return flags;
}

static int dashcdg_rx_park_idle_audio_output_locked(
        struct receiver_state *state,
        uint64_t local_now_ms
) {
    if (!dashcdg_rx_source_idle_and_drained_locked(state, local_now_ms)) {
        return 0;
    }
    if (g_audio == NULL || !g_audio_stream_started) {
        return 0;
    }

    dashcdg_desktop_audio_stop_stream(g_audio);
    dashcdg_desktop_audio_flush_stream_ring(g_audio);
    dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
    state->last_audio_queue_success_local_ms = 0U;
    state->last_audio_timestamp_advance_local_ms = 0U;
    state->last_audio_timestamp_ms = -1;
    state->last_logged_stream_underrun_events = g_audio->stream_underrun_events;
    state->last_observed_stream_underrun_events = g_audio->stream_underrun_events;
    state->last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    return 1;
}

static void dashcdg_rx_configure_audio_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        uint16_t sample_rate,
        uint8_t channels,
        uint8_t frame_ms,
        uint16_t playout_delay_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id
);

static void dashcdg_rx_reset_audio_decode_path_locked(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }

    dashcdg_audio_jitter_clear(&state->audio_jitter);
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    state->jitter_audio_decode_primed = 0;
    state->pcm_src_overlap_valid = 0U;
    state->pcm_src_stream_in_samples = 0U;
    state->pcm_src_stream_out_samples = 0U;
#if defined(DASHCDG_HAVE_LIBSOXR)
    dashcdg_pcm_soxr_stream_reset();
#endif
    state->last_audio_jitter_apply_local_ms = 0U;
    state->last_audio_queue_success_local_ms = 0U;
    state->last_audio_timestamp_advance_local_ms = 0U;
    state->last_audio_timestamp_ms = -1;
    state->audio_resample_trim_ppm = 0;
    state->audio_last_queue_pressure_local_ms = 0U;
    state->audio_servo_enable_after_local_ms = 0U;
    state->rx_audio_applied_valid = 0;

    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
}

static void dashcdg_rx_set_audio_decode_disabled_locked(int disabled) {
    disabled = disabled ? 1 : 0;
    if (g_audio_decode_disabled == disabled) {
        return;
    }

    g_audio_decode_disabled = disabled;
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_flush_stream_ring(g_audio);
        dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        g_receiver.last_observed_stream_underrun_events = g_audio->stream_underrun_events;
        g_receiver.last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
    }
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    dashcdg_rx_reset_audio_decode_path_locked(&g_receiver);
    if (!disabled && g_receiver.network_audio_enabled &&
            g_receiver.announced_audio_sample_rate > 0U &&
            g_receiver.announced_audio_channels > 0U &&
            g_receiver.announced_audio_frame_ms > 0U) {
        dashcdg_rx_configure_audio_locked(
                &g_receiver,
                dashcdg_clock_now_ms(),
                g_receiver.announced_audio_sample_rate,
                g_receiver.announced_audio_channels,
                g_receiver.announced_audio_frame_ms,
                g_receiver.announced_playout_delay_ms,
                g_receiver.announced_audio_profile_id,
                g_receiver.announced_audio_codec_id
        );
        g_rx_force_full_preroll_start = 1;
    }
}

static void dashcdg_rx_toggle_audio_decode_drop(void) {
    int disabled;
    char line[128];

    pthread_mutex_lock(&g_receiver.mutex);
    dashcdg_rx_set_audio_decode_disabled_locked(!g_audio_decode_disabled);
    disabled = g_audio_decode_disabled;
    pthread_mutex_unlock(&g_receiver.mutex);

    snprintf(
            line,
            sizeof(line),
            "[rx] audio decode %s%s",
            disabled ? "disabled" : "enabled",
            disabled ? " (dropping incoming audio packets)" : ""
    );
    dashcdg_rx_async_stdout_line(line);
}

static size_t dashcdg_rx_collect_fault_lines_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        char lines[][256],
        size_t max_lines
) {
    uint64_t delta;
    size_t line_count = 0U;

#define DASHCDG_RX_APPEND_FAULT_LINE(...) \
    do { \
        if (line_count < max_lines) { \
            snprintf(lines[line_count], sizeof(lines[line_count]), __VA_ARGS__); \
            line_count++; \
        } \
    } while (0)

    if (state == NULL) {
        return 0U;
    }
    if (dashcdg_rx_source_idle_and_drained_locked(state, local_now_ms)) {
        state->last_logged_audio_queue_overflows = state->audio_queue_overflows;
        state->last_logged_audio_missing_skips = state->audio_missing_skips;
        state->last_logged_audio_hard_resync_events = state->audio_hard_resync_events;
        state->last_logged_live_missing_skips = state->live_missing_skips;
        state->last_logged_audio_reordered_packets = state->audio_jitter.reordered_packets;
        state->last_logged_cdg_reordered_batches = state->cdg_batch_jitter.reordered_batches;
        if (g_audio != NULL) {
            state->last_logged_stream_underrun_events = g_audio->stream_underrun_events;
        }
        state->last_logged_host_underrun_fault_local_ms = local_now_ms;
        state->last_logged_audio_arrival_gap_events = state->audio_arrival_gap_events;
        state->last_logged_audio_arrival_burst_events = state->audio_arrival_burst_events;
        state->last_logged_audio_arrival_fault_local_ms = local_now_ms;
        return 0U;
    }

    if (state->audio_queue_overflows > state->last_logged_audio_queue_overflows) {
        delta = state->audio_queue_overflows - state->last_logged_audio_queue_overflows;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_queue_overflow +%" DASHCDG_RX_PRIu64 " buf=%u tgt=%u host=%u gate=%s now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) (g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U),
                (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(state),
                (unsigned int) dashcdg_rx_audio_host_latency_ms_locked(),
                dashcdg_rx_audio_recent_auto_recover_locked(state, local_now_ms) ? "auto-recover" : "running",
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_queue_overflows = state->audio_queue_overflows;
    }

    if (state->audio_missing_skips > state->last_logged_audio_missing_skips) {
        delta = state->audio_missing_skips - state->last_logged_audio_missing_skips;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_continuity_skip +%" DASHCDG_RX_PRIu64 " pending=%u buf=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter),
                (unsigned int) (g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U),
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_missing_skips = state->audio_missing_skips;
    }
    if (state->audio_hard_resync_events > state->last_logged_audio_hard_resync_events) {
        delta = state->audio_hard_resync_events - state->last_logged_audio_hard_resync_events;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_hard_resync +%" DASHCDG_RX_PRIu64 " next_seq=%u pending=%u buf=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) state->audio_jitter.next_media_sequence,
                (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter),
                (unsigned int) (g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U),
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_hard_resync_events = state->audio_hard_resync_events;
    }
    if (state->live_missing_skips > state->last_logged_live_missing_skips) {
        delta = state->live_missing_skips - state->last_logged_live_missing_skips;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: cdg_continuity_skip +%" DASHCDG_RX_PRIu64 " pending=%u live=%" DASHCDG_RX_PRIu64 " now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) dashcdg_rx_pending_cdg_count(state),
                (uint64_t) state->live_packets_applied,
                (uint64_t) local_now_ms
        );
        state->last_logged_live_missing_skips = state->live_missing_skips;
    }

    if (state->audio_jitter.reordered_packets > state->last_logged_audio_reordered_packets) {
        delta = state->audio_jitter.reordered_packets - state->last_logged_audio_reordered_packets;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_reorder +%" DASHCDG_RX_PRIu64 " next_seq=%u pending=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) state->audio_jitter.next_media_sequence,
                (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter),
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_reordered_packets = state->audio_jitter.reordered_packets;
    }

    if (state->cdg_batch_jitter.reordered_batches > state->last_logged_cdg_reordered_batches) {
        delta = state->cdg_batch_jitter.reordered_batches - state->last_logged_cdg_reordered_batches;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: cdg_reorder +%" DASHCDG_RX_PRIu64 " next_pkt=%u pending=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (unsigned int) state->cdg_batch_jitter.next_packet_index,
                (unsigned int) dashcdg_rx_pending_cdg_count(state),
                (uint64_t) local_now_ms
        );
        state->last_logged_cdg_reordered_batches = state->cdg_batch_jitter.reordered_batches;
    }

    if (g_audio != NULL && g_audio->stream_underrun_events > state->last_logged_stream_underrun_events) {
        delta = g_audio->stream_underrun_events - state->last_logged_stream_underrun_events;
        if (state->last_logged_host_underrun_fault_local_ms == 0U ||
                local_now_ms <= state->last_logged_host_underrun_fault_local_ms ||
                local_now_ms - state->last_logged_host_underrun_fault_local_ms >=
                        DASHCDG_RX_AUDIO_ARRIVAL_FAULT_LOG_MIN_MS) {
            DASHCDG_RX_APPEND_FAULT_LINE(
                    "[rx] fault: host_underrun +%" DASHCDG_RX_PRIu64 " frames=%" DASHCDG_RX_PRIu64 " buf=%u tgt=%u ts=%d now=%" DASHCDG_RX_PRIu64,
                    (uint64_t) delta,
                    (uint64_t) g_audio->stream_underrun_frames,
                    (unsigned int) dashcdg_desktop_audio_buffered_ms(g_audio),
                    (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(state),
                    (int) DASHCDG_ATOMIC_GET(g_audio->timestamp_ms),
                    (uint64_t) local_now_ms
            );
            state->last_logged_stream_underrun_events = g_audio->stream_underrun_events;
            state->last_logged_host_underrun_fault_local_ms = local_now_ms;
        }
    }
    if ((state->audio_arrival_gap_events > state->last_logged_audio_arrival_gap_events ||
            state->audio_arrival_burst_events > state->last_logged_audio_arrival_burst_events) &&
            state->last_logged_audio_arrival_fault_local_ms != 0U &&
            local_now_ms > state->last_logged_audio_arrival_fault_local_ms &&
            local_now_ms - state->last_logged_audio_arrival_fault_local_ms < DASHCDG_RX_AUDIO_ARRIVAL_FAULT_LOG_MIN_MS) {
        return line_count;
    }
    if (state->audio_arrival_gap_events > state->last_logged_audio_arrival_gap_events) {
        delta = state->audio_arrival_gap_events - state->last_logged_audio_arrival_gap_events;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_arrival_gap +%" DASHCDG_RX_PRIu64 " max=%" DASHCDG_RX_PRIu64 "ms pending=%u buf=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (uint64_t) state->audio_arrival_gap_max_ms,
                (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter),
                (unsigned int) (g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U),
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_arrival_gap_events = state->audio_arrival_gap_events;
        state->audio_arrival_gap_max_ms = 0U;
        state->last_logged_audio_arrival_fault_local_ms = local_now_ms;
    }
    if (state->audio_arrival_burst_events > state->last_logged_audio_arrival_burst_events) {
        delta = state->audio_arrival_burst_events - state->last_logged_audio_arrival_burst_events;
        DASHCDG_RX_APPEND_FAULT_LINE(
                "[rx] fault: audio_arrival_burst +%" DASHCDG_RX_PRIu64 " maxrun=%" DASHCDG_RX_PRIu64 " pending=%u buf=%u now=%" DASHCDG_RX_PRIu64,
                (uint64_t) delta,
                (uint64_t) state->audio_arrival_burst_max_run,
                (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter),
                (unsigned int) (g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U),
                (uint64_t) local_now_ms
        );
        state->last_logged_audio_arrival_burst_events = state->audio_arrival_burst_events;
        state->audio_arrival_burst_max_run = 0U;
        state->last_logged_audio_arrival_fault_local_ms = local_now_ms;
    }

#undef DASHCDG_RX_APPEND_FAULT_LINE
    return line_count;
}

static void dashcdg_rx_emit_fault_lines(char lines[][256], size_t line_count) {
    size_t i;

    for (i = 0U; i < line_count; i++) {
        dashcdg_rx_async_stdout_line(lines[i]);
    }
}

static void dashcdg_rx_note_audio_chunk_arrival_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t gap_ms;

    if (state == NULL) {
        return;
    }
    if (state->audio_skip_hold_until_local_ms != 0U && local_now_ms < state->audio_skip_hold_until_local_ms) {
        state->audio_last_chunk_local_ms = local_now_ms;
        state->audio_burst_window_start_local_ms = local_now_ms;
        state->audio_burst_run_count = 1U;
        return;
    }
    if (state->audio_last_chunk_local_ms != 0U && local_now_ms > state->audio_last_chunk_local_ms) {
        gap_ms = local_now_ms - state->audio_last_chunk_local_ms;
        if (gap_ms >= DASHCDG_RX_AUDIO_ARRIVAL_GAP_THRESHOLD_MS) {
            state->audio_arrival_gap_events++;
            if (gap_ms > state->audio_arrival_gap_max_ms) {
                state->audio_arrival_gap_max_ms = gap_ms;
            }
            state->audio_burst_window_start_local_ms = local_now_ms;
            state->audio_burst_run_count = 1U;
        } else {
            if (state->audio_burst_window_start_local_ms == 0U ||
                    local_now_ms - state->audio_burst_window_start_local_ms > 5U) {
                state->audio_burst_window_start_local_ms = local_now_ms;
                state->audio_burst_run_count = 1U;
            } else {
                state->audio_burst_run_count++;
                if (state->audio_burst_run_count >= DASHCDG_RX_AUDIO_ARRIVAL_BURST_THRESHOLD_MS) {
                    state->audio_arrival_burst_events++;
                    if ((uint64_t) state->audio_burst_run_count > state->audio_arrival_burst_max_run) {
                        state->audio_arrival_burst_max_run = (uint64_t) state->audio_burst_run_count;
                    }
                    state->audio_burst_window_start_local_ms = local_now_ms;
                    state->audio_burst_run_count = 1U;
                }
            }
        }
    } else {
        state->audio_burst_window_start_local_ms = local_now_ms;
        state->audio_burst_run_count = 1U;
    }
    state->audio_last_chunk_local_ms = local_now_ms;
}

static void dashcdg_rx_note_audio_timestamp_progress_locked(
        struct receiver_state *state,
        uint64_t local_now_ms
) {
    int audio_ts;
    int prev_ts;
    int delta_ts;

    if (state == NULL || g_audio == NULL || !g_audio_stream_started) {
        return;
    }

    audio_ts = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    if (audio_ts < 0) {
        return;
    }
    prev_ts = state->last_audio_timestamp_ms;
    if (prev_ts < 0) {
        state->last_audio_timestamp_ms = audio_ts;
        state->last_audio_timestamp_advance_local_ms = local_now_ms;
        return;
    }
    if (audio_ts == prev_ts) {
        return;
    }
    delta_ts = audio_ts - prev_ts;
    if (delta_ts >= 2 || delta_ts <= -2) {
        state->last_audio_timestamp_advance_local_ms = local_now_ms;
    }
    state->last_audio_timestamp_ms = audio_ts;
}

static void dashcdg_rx_reprime_audio_after_host_underrun_locked(struct receiver_state *state) {
    uint64_t now_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled) {
        return;
    }
    now_ms = dashcdg_clock_now_ms();

    state->pcm_src_overlap_valid = 0U;
    state->pcm_src_stream_in_samples = 0U;
    state->pcm_src_stream_out_samples = 0U;
#if defined(DASHCDG_HAVE_LIBSOXR)
    dashcdg_pcm_soxr_stream_reset();
#endif
    state->last_audio_jitter_apply_local_ms = 0U;
    state->last_audio_queue_success_local_ms = 0U;
    state->last_audio_timestamp_advance_local_ms = 0U;
    state->last_audio_timestamp_ms = -1;
    state->jitter_audio_decode_primed = 0;
    if (state->audio_jitter.initialized) {
        dashcdg_audio_jitter_clear(&state->audio_jitter);
    }
    state->audio_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            now_ms,
            dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 1)
    );
    state->audio_servo_enable_after_local_ms = dashcdg_rx_deadline_after_ms(
            now_ms,
            dashcdg_rx_audio_servo_warmup_ms_locked(state)
    );
    state->audio_last_queue_pressure_local_ms = now_ms;

    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_flush_stream_ring(g_audio);
        dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        state->last_observed_stream_underrun_events = g_audio->stream_underrun_events;
        state->last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
        state->last_logged_stream_underrun_events = g_audio->stream_underrun_events;
        state->last_logged_host_underrun_fault_local_ms = now_ms;
    } else {
        state->last_observed_stream_underrun_events = 0U;
        state->last_observed_stream_underrun_frames = 0U;
    }

    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    g_rx_force_full_preroll_start = 1;
}

static int dashcdg_rx_should_auto_recover_host_underrun_locked(
        struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint64_t current_events;
    uint64_t current_frames;
    uint64_t delta_events;
    uint64_t delta_frames;
    uint64_t since_last_queue_success_ms;
    uint64_t since_last_ts_advance_ms;
    uint32_t buffered_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled || state->playback_paused ||
            g_audio == NULL || !g_audio_stream_started || g_audio_start_inflight) {
        return 0;
    }

    current_events = g_audio->stream_underrun_events;
    current_frames = g_audio->stream_underrun_frames;
    if (current_events <= state->last_observed_stream_underrun_events) {
        return 0;
    }

    delta_events = current_events - state->last_observed_stream_underrun_events;
    delta_frames = current_frames - state->last_observed_stream_underrun_frames;
    state->last_observed_stream_underrun_events = current_events;
    state->last_observed_stream_underrun_frames = current_frames;
    state->audio_last_queue_pressure_local_ms = local_now_ms;

    if (state->audio_last_stall_recover_local_ms != 0U &&
            local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    since_last_queue_success_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_queue_success_local_ms);
    since_last_ts_advance_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_timestamp_advance_local_ms);

    /*
     * Avoid recovery chatter: brief callback jitter and short queue dips are common under load and
     * should settle without tearing down stream/ring state.
     */
    if (since_last_queue_success_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS &&
            since_last_ts_advance_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS) {
        return 0;
    }

    if (delta_events < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_EVENTS &&
            delta_frames < (uint64_t) (state->announced_audio_sample_rate / 50U) &&
            buffered_ms > DASHCDG_RX_HOST_UNDERRUN_RECOVER_MAX_BUFFER_MS &&
            since_last_queue_success_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS &&
            since_last_ts_advance_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS) {
        return 0;
    }

    return 1;
}

static int dashcdg_rx_should_auto_recover_zero_buffer_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint32_t buffered_ms;
    size_t pending_audio;
    uint64_t since_last_audio_queue_success_ms;
    uint64_t since_last_dg_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled || state->playback_paused ||
            g_audio == NULL || g_audio_start_inflight) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    if (buffered_ms != 0U) {
        return 0;
    }

    if (state->audio_last_stall_recover_local_ms != 0U &&
            local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS) {
        return 0;
    }

    since_last_audio_queue_success_ms = dashcdg_rx_elapsed_ms_safe(
            local_now_ms,
            state->last_audio_queue_success_local_ms
    );
    if (since_last_audio_queue_success_ms < DASHCDG_RX_ZERO_BUFFER_STALL_RECOVER_MS) {
        return 0;
    }

    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_datagram_local_ms);
    if (since_last_dg_ms > 1000U) {
        return 0;
    }

    pending_audio = dashcdg_audio_jitter_occupied_count(&state->audio_jitter);
    if (pending_audio == 0U && !state->audio_jitter.initialized) {
        return 0;
    }

    return 1;
}

static int dashcdg_rx_should_auto_recover_buffered_silent_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint32_t buffered_ms;
    uint64_t since_last_ts_advance_ms;
    uint64_t since_last_dg_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled || state->playback_paused ||
            g_audio == NULL || !g_audio_stream_started || g_audio_start_inflight) {
        return 0;
    }
    if (state->audio_last_stall_recover_local_ms != 0U &&
            local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS) {
        return 0;
    }
    if (state->last_audio_timestamp_advance_local_ms == 0U) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    if (buffered_ms == 0U) {
        return 0;
    }

    since_last_ts_advance_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_timestamp_advance_local_ms);
    if (since_last_ts_advance_ms < DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS) {
        return 0;
    }

    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_datagram_local_ms);
    if (since_last_dg_ms > 1000U) {
        return 0;
    }

    return 1;
}

static int dashcdg_rx_should_auto_recover_decode_stall_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint64_t since_last_apply_ms;
    uint64_t since_last_dg_ms;
    size_t pending_audio;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled || state->playback_paused ||
            g_audio == NULL || g_audio_start_inflight || !state->audio_jitter.initialized) {
        return 0;
    }
    if (state->audio_last_stall_recover_local_ms != 0U &&
            local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS) {
        return 0;
    }

    pending_audio = dashcdg_audio_jitter_occupied_count(&state->audio_jitter);
    if (pending_audio < DASHCDG_RX_DECODE_STALL_MIN_PENDING_SLOTS) {
        return 0;
    }

    since_last_apply_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_jitter_apply_local_ms);
    if (since_last_apply_ms < DASHCDG_RX_DECODE_STALL_RECOVER_MS) {
        return 0;
    }

    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_datagram_local_ms);
    if (since_last_dg_ms > 500U) {
        return 0;
    }

    return 1;
}

static int dashcdg_rx_should_force_post_track_recover_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms
) {
    uint64_t since_session_change_ms;
    uint64_t since_last_ts_advance_ms;
    uint64_t since_last_queue_success_ms;
    uint64_t since_last_dg_ms;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled ||
            state->playback_paused || g_audio == NULL || !g_audio_stream_started || g_audio_start_inflight ||
            state->last_session_change_local_ms == 0U) {
        return 0;
    }
    since_session_change_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_session_change_local_ms);
    if (since_session_change_ms > DASHCDG_RX_POST_TRACK_RECOVER_WINDOW_MS) {
        return 0;
    }
    if (state->audio_last_stall_recover_local_ms != 0U &&
            local_now_ms - state->audio_last_stall_recover_local_ms < DASHCDG_RX_ZERO_BUFFER_RECOVER_COOLDOWN_MS) {
        return 0;
    }
    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_datagram_local_ms);
    if (since_last_dg_ms > 700U) {
        return 0;
    }
    since_last_ts_advance_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_timestamp_advance_local_ms);
    since_last_queue_success_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_audio_queue_success_local_ms);
    if (since_last_ts_advance_ms < DASHCDG_RX_POST_TRACK_RECOVER_STALE_MS &&
            since_last_queue_success_ms < DASHCDG_RX_POST_TRACK_RECOVER_STALE_MS) {
        return 0;
    }
    return 1;
}

static void dashcdg_rx_rebuild_audio_decode_path_locked(struct receiver_state *state, uint64_t local_now_ms) {
    if (state == NULL || g_audio_decode_disabled) {
        return;
    }

    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_flush_stream_ring(g_audio);
        dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        state->last_observed_stream_underrun_events = g_audio->stream_underrun_events;
        state->last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
        state->last_logged_stream_underrun_events = g_audio->stream_underrun_events;
    } else {
        state->last_observed_stream_underrun_events = 0U;
        state->last_observed_stream_underrun_frames = 0U;
        state->last_logged_stream_underrun_events = 0U;
    }
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    g_rx_force_full_preroll_start = 1;

    dashcdg_rx_reset_audio_decode_path_locked(state);
    state->audio_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 1)
    );
    state->audio_servo_enable_after_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_audio_servo_warmup_ms_locked(state)
    );

    if (state->network_audio_enabled &&
            state->announced_audio_sample_rate > 0U &&
            state->announced_audio_channels > 0U &&
            state->announced_audio_frame_ms > 0U) {
        dashcdg_rx_configure_audio_locked(
                state,
                local_now_ms,
                state->announced_audio_sample_rate,
                state->announced_audio_channels,
                state->announced_audio_frame_ms,
                state->announced_playout_delay_ms,
                state->announced_audio_profile_id,
                state->announced_audio_codec_id
        );
    }
}

static int dashcdg_rx_apply_graphics_trim_ms(int playback_ms) {
    int64_t v = (int64_t) playback_ms + (int64_t) g_rx_graphics_trim_ms;

    if (v < 0) {
        return 0;
    }
    if (v > (int64_t) INT_MAX) {
        return INT_MAX;
    }
    return (int) v;
}

static uint32_t dashcdg_rx_estimated_audio_pipeline_lag_ms_locked(const struct receiver_state *state) {
    uint32_t lag_ms = 0U;

    if (g_audio != NULL) {
        lag_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
        lag_ms += dashcdg_desktop_audio_output_latency_ms(g_audio);
    }

    /*
     * Before DAC timestamps exist, bootstrap sender-clock graphics with the same
     * startup cushion the RX uses for audio claim/start. Once the DAC is running,
     * measured skew wins instead.
     */
    if (lag_ms == 0U && state != NULL) {
        lag_ms = dashcdg_rx_audio_target_total_latency_ms_locked(state);
    }

    if (lag_ms > 1000U) {
        lag_ms = 1000U;
    }
    return lag_ms;
}

static int dashcdg_rx_sender_graphics_playback_now_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint64_t *out_playback_ms
) {
    uint64_t sender_playback_ms = 0U;
    uint64_t local_audio_playback_ms = 0U;
    uint32_t lag_ms;
    uint64_t adjusted_sender_ms;

    if (out_playback_ms == NULL ||
            !dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
        return 0;
    }

    if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms) &&
            sender_playback_ms > local_audio_playback_ms) {
        uint64_t skew_ms = sender_playback_ms - local_audio_playback_ms;

        lag_ms = skew_ms >= 1000U ? 1000U : (uint32_t) skew_ms;
    } else {
        lag_ms = dashcdg_rx_estimated_audio_pipeline_lag_ms_locked(state);
    }

    if (sender_playback_ms > (uint64_t) lag_ms) {
        adjusted_sender_ms = sender_playback_ms - (uint64_t) lag_ms;
    } else {
        adjusted_sender_ms = 0U;
    }

    /*
     * Sender-clock graphics are useful for cross-receiver parity, but they must not outrun the
     * locally heard DAC by whole lyric words. If our sender-lag model still lands ahead of the
     * DAC clock, clamp to the local audio timeline.
     */
    if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms) &&
            adjusted_sender_ms > local_audio_playback_ms) {
        adjusted_sender_ms = local_audio_playback_ms;
    }

    *out_playback_ms = adjusted_sender_ms;
    return 1;
}

static int dashcdg_rx_playback_ms_for_graphics_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t local_audio_playback_ms = 0U;
    uint64_t sender_playback_ms = 0U;
    int base = -1;

    if (state == NULL) {
        return -1;
    }
    if (g_rx_graphics_clock_sender) {
        if (dashcdg_rx_sender_graphics_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(sender_playback_ms);
        } else if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(local_audio_playback_ms);
        }
    } else {
        if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(local_audio_playback_ms);
        } else if (dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(sender_playback_ms);
        }
    }
    if (base < 0) {
        return -1;
    }
    return dashcdg_rx_apply_graphics_trim_ms(base);
}

/*
 * Network CDG deltas are incremental. If the first wire batches arrive before a snapshot/anchor
 * has seeded live_state (cold RX-before-TX), publish_render_snapshot still shows the asset reader;
 * once live_packets_applied > 0 we switch to live_state — which was still empty → black until a
 * keyframe. Copy reader (or v4 bridge canvas) before processing the first live batch.
 */
static void dashcdg_rx_seed_live_state_before_first_wire_delta_locked(
        struct receiver_state *state,
        uint64_t local_now_ms
) {
    int playback_ms;

    if (state == NULL || state->playback_paused || state->live_packets_applied != 0U) {
        return;
    }

    if (state->v4_bridge_cdg_valid) {
        state->live_state = state->v4_bridge_cdg;
        return;
    }

    if (!state->reader_ready) {
        return;
    }

    playback_ms = dashcdg_rx_playback_ms_for_graphics_locked(state, local_now_ms);
    if (playback_ms < 0) {
        return;
    }

    dashcdg_cdg_reader_seek(&state->reader, dashcdg_ms_to_packet_count((uint64_t) playback_ms));
    state->live_state = state->reader.state;
}

static void dashcdg_rx_publish_render_snapshot_locked(uint64_t local_now_ms) {
    struct dashcdg_rx_render_snapshot snapshot;
    int playback_ms;
    uint64_t dac_ms = 0U;
    uint64_t snd_ms = 0U;
    int have_dac;
    int have_snd;

    memset(&snapshot, 0, sizeof(snapshot));
    playback_ms = dashcdg_rx_playback_ms_for_graphics_locked(&g_receiver, local_now_ms);
    if (playback_ms < 0) {
        playback_ms = 0;
    }

    if (g_rx_av_sync_log_ms > 0U && g_audio != NULL) {
        if (g_rx_last_av_sync_log_ms == 0U ||
                local_now_ms - g_rx_last_av_sync_log_ms >= (uint64_t) g_rx_av_sync_log_ms) {
            g_rx_last_av_sync_log_ms = local_now_ms;
            have_dac = dashcdg_rx_local_audio_playback_now_locked(&dac_ms);
            have_snd = dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &snd_ms);
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif
            RX_ERR(
                    "[rx-av-sync] dac=%d:%" DASHCDG_RX_PRIu64 " sender=%d:%" DASHCDG_RX_PRIu64
                    " snap=%d q=%ums tgt=%u host=%u trim=%dppm clock=%s\n",
                    have_dac,
                    (unsigned long long) dac_ms,
                    have_snd,
                    (unsigned long long) snd_ms,
                    playback_ms,
                    (unsigned int) dashcdg_desktop_audio_buffered_ms(g_audio),
                    (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver),
                    (unsigned int) dashcdg_rx_audio_host_latency_ms_locked(),
                    (int) g_receiver.audio_resample_trim_ppm,
                    g_rx_graphics_clock_sender ? "sender" : "dac"
            );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        }
    }

    snapshot.valid = 1;
    snapshot.playback_ms = playback_ms;
    /*
     * Decoded v4 anchor fills v4_bridge_cdg before apply_snapshot_locked may run (should_apply gate).
     * Prefer that canvas until the first snapshot commits; then keep the historical rule (bridge
     * while no live deltas applied yet).
     */
    if (g_receiver.v4_bridge_cdg_valid && g_receiver.cdg_snapshots_applied == 0U) {
        snapshot.state = g_receiver.v4_bridge_cdg;
    } else if (g_receiver.live_packets_applied == 0U &&
            g_receiver.cdg_snapshots_applied == 0U &&
            g_receiver.v4_bridge_cdg_valid) {
        snapshot.state = g_receiver.v4_bridge_cdg;
    } else {
        snapshot.state = g_receiver.live_state;
    }

    pthread_mutex_lock(&g_render_mutex);
    g_render_snapshot = snapshot;
    pthread_mutex_unlock(&g_render_mutex);
}

static void dashcdg_rx_clear_fec_group(struct dashcdg_rx_fec_group *group) {
    if (group == NULL) {
        return;
    }

    memset(group, 0, sizeof(*group));
}

static struct dashcdg_rx_fec_group *dashcdg_rx_get_fec_group_locked(
        struct dashcdg_rx_fec_group groups[],
        uint32_t group_id
) {
    struct dashcdg_rx_fec_group *free_group = NULL;
    struct dashcdg_rx_fec_group *oldest_group = NULL;

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        if (groups[i].occupied) {
            if (groups[i].group_id == group_id) {
                return &groups[i];
            }
            if (oldest_group == NULL || groups[i].group_id < oldest_group->group_id) {
                oldest_group = &groups[i];
            }
        } else if (free_group == NULL) {
            free_group = &groups[i];
        }
    }

    if (free_group != NULL) {
        dashcdg_rx_clear_fec_group(free_group);
        free_group->occupied = 1;
        free_group->group_id = group_id;
        return free_group;
    }

    if (oldest_group != NULL) {
        dashcdg_rx_clear_fec_group(oldest_group);
        oldest_group->occupied = 1;
        oldest_group->group_id = group_id;
        return oldest_group;
    }

    return NULL;
}

static void dashcdg_rx_purge_audio_fec_locked(struct receiver_state *state) {
    uint8_t group_span;

    if (state == NULL || !state->audio_jitter.initialized) {
        return;
    }

    group_span = state->announced_audio_fec_group_size;
    if (group_span == 0) {
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        struct dashcdg_rx_fec_group *group = &state->audio_fec_groups[i];
        uint32_t first_sequence;
        uint32_t end_sequence;

        if (!group->occupied || group->expected_group_size == 0) {
            continue;
        }

        first_sequence = group->group_id * (uint32_t) group_span + 1U;
        end_sequence = first_sequence + group->expected_group_size;
        if (state->audio_jitter.next_media_sequence >= end_sequence) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static void dashcdg_rx_purge_cdg_fec_locked(struct receiver_state *state) {
    uint8_t group_span;

    if (state == NULL || !state->cdg_batch_jitter.initialized) {
        return;
    }

    group_span = state->announced_cdg_fec_group_size;
    if (group_span == 0) {
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        struct dashcdg_rx_fec_group *group = &state->cdg_fec_groups[i];
        uint64_t first_batch_index;
        uint64_t end_packet_index;

        if (!group->occupied || group->expected_group_size == 0) {
            continue;
        }

        first_batch_index = (uint64_t) group->group_id * (uint64_t) group_span;
        end_packet_index = (first_batch_index + (uint64_t) group->expected_group_size) * DASHCDG_MAX_CDG_BATCH_PACKETS;
        if (state->cdg_batch_jitter.next_packet_index >= end_packet_index) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static int dashcdg_rx_insert_audio_pending_locked(
        struct receiver_state *state,
        uint32_t media_sequence,
        uint64_t playback_ms,
        uint8_t frame_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id,
        const uint8_t *payload,
        uint16_t payload_length,
        int count_reorder
) {
    if (state == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    return dashcdg_audio_jitter_insert(
            &state->audio_jitter,
            media_sequence,
            playback_ms,
            frame_ms,
            audio_profile_id,
            codec_id,
            payload,
            payload_length,
            count_reorder
    );
}

static int dashcdg_rx_insert_cdg_pending_locked(
        struct receiver_state *state,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_reorder
) {
    int inserted;

    if (state == NULL || payload == NULL || packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    if (dashcdg_cdg_batch_jitter_capacity(&state->cdg_batch_jitter) == 0U) {
        return 0;
    }

    /* No proactive evict: evict drops furthest-ahead slots; a nearly-full ring is normal on LAN and
     * throwing away that tail desyncs CDG from the snapshot anchor. Evict only on insert failure. */
    inserted = dashcdg_cdg_batch_jitter_insert(
            &state->cdg_batch_jitter,
            packet_start_index,
            packet_count,
            payload,
            count_reorder
    );
    if (!inserted) {
        dashcdg_cdg_batch_jitter_evict_pressure(&state->cdg_batch_jitter, 2U);
        inserted = dashcdg_cdg_batch_jitter_insert(
                &state->cdg_batch_jitter,
                packet_start_index,
                packet_count,
                payload,
                count_reorder
        );
    }
    return inserted;
}

static uint8_t dashcdg_rx_gf256_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0U;
    for (uint8_t i = 0U; i < 8U; ++i) {
        if ((b & 1U) != 0U) {
            p ^= a;
        }
        if ((a & 0x80U) != 0U) {
            a = (uint8_t) ((a << 1U) ^ 0x1DU);
        } else {
            a <<= 1U;
        }
        b >>= 1U;
    }
    return p;
}

static uint8_t dashcdg_rx_gf256_pow(uint8_t base, uint8_t exp) {
    uint8_t out = 1U;
    while (exp-- > 0U) {
        out = dashcdg_rx_gf256_mul(out, base);
    }
    return out;
}

static uint8_t dashcdg_rx_gf256_inv(uint8_t v) {
    if (v == 0U) {
        return 0U;
    }
    return dashcdg_rx_gf256_pow(v, 254U);
}

static uint8_t dashcdg_rx_cdg_repair_coeff(uint8_t member_index, uint8_t parity_index) {
    return dashcdg_rx_gf256_pow((uint8_t) (member_index + 1U), parity_index);
}

static int dashcdg_rx_gf256_solve(
        uint8_t n,
        const uint8_t a_in[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX],
        const uint8_t b_in[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX],
        uint8_t x_out[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX]
) {
    uint8_t a[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t b[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t row = 0U;
    if (n == 0U || n > DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
        return 0;
    }
    memcpy(a, a_in, sizeof(a));
    memcpy(b, b_in, sizeof(b));
    while (row < n) {
        uint8_t pivot = row;
        while (pivot < n && a[pivot][row] == 0U) {
            pivot++;
        }
        if (pivot >= n) {
            return 0;
        }
        if (pivot != row) {
            for (uint8_t c = 0U; c < n; ++c) {
                uint8_t t = a[row][c];
                a[row][c] = a[pivot][c];
                a[pivot][c] = t;
            }
            {
                uint8_t tb = b[row];
                b[row] = b[pivot];
                b[pivot] = tb;
            }
        }
        {
            uint8_t inv = dashcdg_rx_gf256_inv(a[row][row]);
            if (inv == 0U) {
                return 0;
            }
            for (uint8_t c = 0U; c < n; ++c) {
                a[row][c] = dashcdg_rx_gf256_mul(a[row][c], inv);
            }
            b[row] = dashcdg_rx_gf256_mul(b[row], inv);
        }
        for (uint8_t r = 0U; r < n; ++r) {
            if (r == row || a[r][row] == 0U) {
                continue;
            }
            {
                uint8_t factor = a[r][row];
                for (uint8_t c = 0U; c < n; ++c) {
                    a[r][c] ^= dashcdg_rx_gf256_mul(factor, a[row][c]);
                }
                b[r] ^= dashcdg_rx_gf256_mul(factor, b[row]);
            }
        }
        row++;
    }
    memcpy(x_out, b, n);
    return 1;
}

static void dashcdg_rx_try_recover_audio_group_locked(
        struct receiver_state *state,
        struct dashcdg_rx_fec_group *group
) {
    const uint8_t *known_payloads[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint16_t known_lengths[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t recovered_payload[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    uint16_t recovered_length = 0;
    size_t known_count = 0;
    int missing_index = -1;
    uint32_t media_sequence;
    uint64_t playback_ms;

    if (state == NULL || group == NULL || !group->occupied || !group->parity_present || group->expected_group_size <= 1) {
        return;
    }

    for (uint8_t i = 0; i < group->expected_group_size; ++i) {
        if (group->member_present[i]) {
            known_payloads[known_count] = group->member_payloads[i];
            known_lengths[known_count] = group->member_lengths[i];
            known_count++;
        } else if (missing_index < 0) {
            missing_index = (int) i;
        } else {
            return;
        }
    }

    if (missing_index < 0 || known_count + 1U != group->expected_group_size || state->announced_audio_fec_group_size == 0) {
        return;
    }

    if (!dashcdg_fec_parity_recover(&group->parity, known_payloads, known_lengths, known_count, recovered_payload, &recovered_length)) {
        state->fec_recovery_failures++;
        group->parity_present = 0;
        return;
    }

    media_sequence = group->group_id * (uint32_t) state->announced_audio_fec_group_size + (uint32_t) missing_index + 1U;
    playback_ms = (uint64_t) (media_sequence - 1U) * (uint64_t) state->announced_audio_frame_ms;
    if (dashcdg_rx_insert_audio_pending_locked(
                state,
                media_sequence,
                playback_ms,
                state->announced_audio_frame_ms,
                state->announced_audio_profile_id,
                state->announced_audio_codec_id ? state->announced_audio_codec_id : DASHCDG_V4_AUDIO_CODEC_OPUS,
                recovered_payload,
                recovered_length,
                0
        )) {
        group->member_present[missing_index] = 1;
        group->member_lengths[missing_index] = recovered_length;
        memcpy(group->member_payloads[missing_index], recovered_payload, recovered_length);
        state->fec_audio_recovered++;
    }
}

static void dashcdg_rx_try_recover_cdg_group_locked(
        struct receiver_state *state,
        struct dashcdg_rx_fec_group *group
) {
    int missing[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t miss_count = 0U;
    uint8_t symbol_bytes = group != NULL ? group->parity_symbol_bytes : 0U;
    uint8_t parity_ids[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t parity_count = 0U;
    uint8_t known[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    if (state == NULL || group == NULL || !group->occupied || group->expected_group_size <= 1U || symbol_bytes == 0U ||
            symbol_bytes > DASHCDG_MAX_FEC_PAYLOAD_BYTES || state->announced_cdg_fec_group_size == 0U) {
        return;
    }
    for (uint8_t pi = 0U; pi < DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX; ++pi) {
        if ((group->parity_present_mask & (uint8_t) (1U << pi)) != 0U) {
            parity_ids[parity_count++] = pi;
            memset(known[pi], 0, symbol_bytes);
        }
    }
    for (uint8_t i = 0U; i < group->expected_group_size; ++i) {
        if (group->member_present[i]) {
            uint8_t sym[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
            memset(sym, 0, symbol_bytes);
            sym[0] = (uint8_t) group->member_lengths[i];
            memcpy(sym + 1U, group->member_payloads[i], group->member_lengths[i]);
            for (uint8_t p = 0U; p < parity_count; ++p) {
                uint8_t pid = parity_ids[p];
                uint8_t coeff = dashcdg_rx_cdg_repair_coeff(i, pid);
                for (uint8_t b = 0U; b < symbol_bytes; ++b) {
                    known[pid][b] ^= dashcdg_rx_gf256_mul(coeff, sym[b]);
                }
            }
        } else if (miss_count < DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE) {
            missing[miss_count++] = (int) i;
        }
    }
    if (miss_count == 0U) {
        return;
    }
    {
        uint16_t missing_mask = 0U;
        for (uint8_t m = 0U; m < miss_count; ++m) {
            if (missing[m] >= 0 && missing[m] < 16) {
                missing_mask |= (uint16_t) (1U << (uint8_t) missing[m]);
            }
        }
        if (missing_mask != 0U) {
            dashcdg_rx_send_v4_repair_nack_locked(
                    dashcdg_clock_now_ms(),
                    DASHCDG_STREAM_TYPE_CDG,
                    group->group_id,
                    group->expected_group_size,
                    missing_mask
            );
        }
    }
    if (parity_count < miss_count) {
        return;
    }
    if (miss_count > DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
        miss_count = DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX;
    }
    {
        uint8_t coeff_matrix[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t rhs[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t sol[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t rec_syms[DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX][DASHCDG_MAX_FEC_PAYLOAD_BYTES];
        memset(rec_syms, 0, sizeof(rec_syms));
        for (uint8_t b = 0U; b < symbol_bytes; ++b) {
            for (uint8_t r = 0U; r < miss_count; ++r) {
                uint8_t pid = parity_ids[r];
                rhs[r] = (uint8_t) (group->parity_symbols[pid][b] ^ known[pid][b]);
                for (uint8_t c = 0U; c < miss_count; ++c) {
                    coeff_matrix[r][c] = dashcdg_rx_cdg_repair_coeff((uint8_t) missing[c], pid);
                }
            }
            if (!dashcdg_rx_gf256_solve(miss_count, coeff_matrix, rhs, sol)) {
                state->fec_recovery_failures++;
                state->cdg_unrecoverable_groups++;
                return;
            }
            for (uint8_t c = 0U; c < miss_count; ++c) {
                rec_syms[c][b] = sol[c];
            }
        }
        for (uint8_t c = 0U; c < miss_count; ++c) {
            int mi = missing[c];
            uint16_t len = rec_syms[c][0];
            uint8_t pc;
            uint64_t bi;
            uint64_t psi;
            if (len == 0U || len > DASHCDG_MAX_FEC_PAYLOAD_BYTES - 1U || (len % DASHCDG_SUBCHANNEL_PACKET_BYTES) != 0U) {
                state->fec_recovery_failures++;
                state->cdg_unrecoverable_groups++;
                return;
            }
            pc = (uint8_t) (len / DASHCDG_SUBCHANNEL_PACKET_BYTES);
            if (pc == 0U || pc > DASHCDG_MAX_CDG_BATCH_PACKETS) {
                state->fec_recovery_failures++;
                state->cdg_unrecoverable_groups++;
                return;
            }
            bi = (uint64_t) group->group_id * (uint64_t) state->announced_cdg_fec_group_size + (uint64_t) mi;
            psi = bi * DASHCDG_MAX_CDG_BATCH_PACKETS;
            if (!dashcdg_rx_insert_cdg_pending_locked(state, psi, pc, &rec_syms[c][1], 0)) {
                state->fec_recovery_failures++;
                state->cdg_unrecoverable_groups++;
                return;
            }
            group->member_present[mi] = 1;
            group->member_lengths[mi] = len;
            memcpy(group->member_payloads[mi], &rec_syms[c][1], len);
            state->fec_cdg_recovered++;
        }
    }
}

static void dashcdg_rx_observe_audio_payload_for_fec_locked(
        struct receiver_state *state,
        uint32_t group_id,
        uint8_t group_index,
        const uint8_t *encoded_bytes,
        uint16_t encoded_length
) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;

    if (state == NULL || encoded_bytes == NULL || state->announced_audio_fec_group_size <= 1 ||
            state->announced_audio_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            group_index >= state->announced_audio_fec_group_size ||
            encoded_length == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_audio_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[group_index]) {
        group->member_present[group_index] = 1;
        group->member_lengths[group_index] = encoded_length;
        memcpy(
                group->member_payloads[group_index],
                encoded_bytes,
                encoded_length
        );
    }

    dashcdg_rx_try_recover_audio_group_locked(state, group);
}

static void dashcdg_rx_observe_audio_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_observe_audio_payload_for_fec_locked(
            state,
            view->audio_frame.group_id,
            view->audio_frame.group_index,
            view->audio_frame.encoded_bytes,
            view->audio_frame.encoded_length
    );
}

static void dashcdg_rx_observe_cdg_payload_for_fec_locked(
        struct receiver_state *state,
        uint32_t group_id,
        uint8_t group_index,
        uint8_t packet_count,
        const uint8_t *packet_bytes
) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;
    uint16_t payload_length;

    if (state == NULL || packet_bytes == NULL || state->announced_cdg_fec_group_size <= 1 ||
            state->announced_cdg_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            group_index >= state->announced_cdg_fec_group_size ||
            packet_count == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_cdg_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[group_index]) {
        payload_length = (uint16_t) ((size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES);
        group->member_present[group_index] = 1;
        group->member_lengths[group_index] = payload_length;
        memcpy(
                group->member_payloads[group_index],
                packet_bytes,
                payload_length
        );
    }

    dashcdg_rx_try_recover_cdg_group_locked(state, group);
}

static void dashcdg_rx_observe_cdg_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_observe_cdg_payload_for_fec_locked(
            state,
            view->cdg_batch.group_id,
            view->cdg_batch.group_index,
            view->cdg_batch.packet_count,
            view->cdg_batch.packet_bytes
    );
}

static void dashcdg_rx_observe_fec_parity_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group = NULL;

    if (state == NULL || view == NULL || view->fec_parity.group_size <= 1 ||
            view->fec_parity.group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->fec_parity.payload_bytes == 0 || view->fec_parity.payload_xor == NULL) {
        return;
    }
    if (dashcdg_rx_is_stale_prior_session_media_locked(state, view)) {
        return;
    }

    if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO && g_audio_decode_disabled) {
        return;
    }

    if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, view->fec_parity.group_id);
    } else if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, view->fec_parity.group_id);
    }
    if (group == NULL) {
        return;
    }

    group->expected_group_size = view->fec_parity.group_size;
    group->parity_present = 1;
    dashcdg_fec_parity_init(&group->parity);
    group->parity.payload_bytes = view->fec_parity.payload_bytes;
    group->parity.payload_length_xor = view->fec_parity.payload_length_xor;
    memcpy(group->parity.payload_xor, view->fec_parity.payload_xor, view->fec_parity.payload_bytes);

    if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        dashcdg_rx_try_recover_audio_group_locked(state, group);
    } else if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        dashcdg_rx_try_recover_cdg_group_locked(state, group);
    }
}

static int dashcdg_rx_is_stale_prior_session_media_locked(
        const struct receiver_state *state,
        const struct dashcdg_packet_view *view
) {
    uint64_t warmup_window_ms;
    int looks_like_prior_session_wall;

    if (state == NULL || view == NULL || state->session_start_ms == 0U) {
        return 0;
    }

    warmup_window_ms = state->announced_playout_delay_ms > 0U
            ? (uint64_t) state->announced_playout_delay_ms
            : (uint64_t) DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;

    looks_like_prior_session_wall =
            view->header.sender_time_ms + warmup_window_ms < state->session_start_ms;
    if (!looks_like_prior_session_wall) {
        return 0;
    }
    /*
     * TX often sets session_start_ms to playback_anchor (now + warmup) while datagrams still carry
     * sender_time_ms ≈ wall now. That satisfies the inequality above even for fresh media — gate
     * with the session_info sender_time that established this session_start.
     */
    if (state->v4_session_epoch_anchor_sender_ms != 0U &&
            view->header.sender_time_ms + DASHCDG_RX_V4_SESSION_REORDER_SENDER_SLACK_MS >=
                    state->v4_session_epoch_anchor_sender_ms) {
        return 0;
    }
    return 1;
}

static int dashcdg_rx_store_audio_frame_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->audio_frame.encoded_bytes == NULL) {
        return 0;
    }

    if (!dashcdg_rx_insert_audio_pending_locked(
                state,
                view->audio_frame.media_sequence,
                view->audio_frame.playback_ms,
                view->audio_frame.frame_ms,
                DASHCDG_V4_AUDIO_PROFILE_QUALITY,
                DASHCDG_V4_AUDIO_CODEC_OPUS,
                view->audio_frame.encoded_bytes,
                view->audio_frame.encoded_length,
                1
        )) {
        return 0;
    }

    dashcdg_rx_observe_audio_for_fec_locked(state, view);
    return 1;
}

static int dashcdg_rx_store_cdg_batch_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->cdg_batch.packet_bytes == NULL) {
        return 0;
    }

    if (!dashcdg_rx_insert_cdg_pending_locked(
                state,
                view->cdg_batch.packet_start_index,
                view->cdg_batch.packet_count,
                view->cdg_batch.packet_bytes,
                1
        )) {
        return 0;
    }

    dashcdg_rx_observe_cdg_for_fec_locked(state, view);
    return 1;
}

static void dashcdg_rx_pcm48_mono_to_interleaved_stereo(
        const int16_t *mono,
        int16_t *stereo_interleaved,
        size_t mono_sample_count
) {
    size_t i;

    for (i = 0; i < mono_sample_count; ++i) {
        int16_t s = mono[i];

        stereo_interleaved[i * 2U] = s;
        stereo_interleaved[i * 2U + 1U] = s;
    }
}

/*
 * Session/wire may be mono (TX default). Open the host device as stereo and L=R upmix after decode:
 * mono Pa_OpenStream on some Windows WASAPI/shared stacks has produced bad routing / overload.
 * Decoder channel count stays on the wire; only the device ring uses this mapping.
 */
static uint16_t dashcdg_rx_portaudio_output_channels(uint8_t wire_channels) {
    if (wire_channels == 1U) {
        return 2U;
    }
    if (wire_channels == 0U) {
        return 0U;
    }
    return (uint16_t) wire_channels;
}

/*
 * Returns 1 if PCM was queued (full or partial), 0 if PortAudio took no samples
 * (retry the same jitter frame), -1 if decode failed (caller must advance/drop).
 */
/* Opus allows up to 120 ms frames @ 48 kHz → 5760 samples/channel (see TX session audio_frame_ms). */
#define DASHCDG_RX_OPUS_MONO_SCRATCH_SAMPLES 5760
#define DASHCDG_RX_PCM_WORK_SAMPLES_MAX (DASHCDG_RX_OPUS_MONO_SCRATCH_SAMPLES + DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES)
#define DASHCDG_RX_PCM_INTERLEAVED_SAMPLES_MAX (DASHCDG_RX_OPUS_MONO_SCRATCH_SAMPLES * 2U)

static int dashcdg_rx_queue_decoded_interleaved_pcm_locked(
        struct receiver_state *state,
        int decoded_frames,
        int16_t pcm[DASHCDG_RX_PCM_INTERLEAVED_SAMPLES_MAX],
        int16_t mono_scratch[DASHCDG_RX_PCM_WORK_SAMPLES_MAX],
        uint16_t host_output_channels,
        int32_t trim_ppm_for_frame,
        int have_trim_ppm_for_frame,
        uint64_t playback_ms,
        uint8_t wire_frame_ms,
        uint8_t codec_id
) {
    size_t queued_frames = 0U;
    size_t expected_queued_fc = 0U;

    {
        uint32_t ses_sr;
        uint32_t out_sr;
        uint32_t effective_out_sr;
        uint32_t queued_ms_estimate = 0U;
        uint32_t target_buffer_ms = dashcdg_rx_audio_target_buffer_ms_locked(state);
        uint32_t queue_limit_ms = target_buffer_ms;
        int resampled_output = 0;
        uint32_t buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
        int32_t trim_ppm = have_trim_ppm_for_frame
                ? trim_ppm_for_frame
                : dashcdg_rx_audio_resample_trim_ppm_locked(state, buffered_ms);
        int allow_sync_trim_equal_rate = 0;
        int32_t equal_rate_trim_ppm = 0;
        unsigned int host_ch = (unsigned int) host_output_channels;
        const int16_t *qptr = pcm;
        size_t qfc = (size_t) decoded_frames;
        int16_t *rs_tmp = NULL;
        int16_t wr_r_st[DASHCDG_RX_PCM_WORK_SAMPLES_MAX];

        dashcdg_desktop_audio_refresh_stream_sample_rate(g_audio);
        ses_sr = dashcdg_desktop_audio_session_sample_rate(g_audio);
        out_sr = dashcdg_desktop_audio_output_sample_rate(g_audio);
        /*
         * Keep the RX path bit-transparent whenever decode/session and device rates already match.
         * This queue path is shared by PortAudio and WinMM outputs; forcing SRC/trim in-rate can
         * produce "healthy logs but audible silence" on some Windows endpoints.
         *
         * Exception: when group-sync is ACTIVE and controller trim is non-zero, allow a tiny
         * equal-rate stretch/compress path (SOXR stream) to converge multi-receiver sample timing.
         */
        allow_sync_trim_equal_rate =
                (ses_sr == out_sr) &&
                (state->sync_group_mode == DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE) &&
                (trim_ppm != 0);
        if (ses_sr == out_sr && !allow_sync_trim_equal_rate) {
            trim_ppm = 0;
            state->pcm_src_overlap_valid = 0U;
            state->audio_equal_rate_slip_accum = 0;
        } else if (allow_sync_trim_equal_rate) {
            /*
             * Keep equal-rate sync correction off libsoxr to avoid runtime instability in
             * soxr_deinterleave under rapid move/resize + active playback. Apply a bounded
             * frame-slip correction below instead.
             */
            equal_rate_trim_ppm = trim_ppm;
            trim_ppm = 0;
        }
        effective_out_sr = out_sr;
        if (out_sr > 0U && trim_ppm != 0) {
            int64_t scaled = (int64_t) out_sr * (int64_t) (1000000 + trim_ppm);
            uint64_t adjusted;

            if (scaled <= 0) {
                scaled = (int64_t) out_sr * 1000000LL;
            }
            adjusted = (uint64_t) ((scaled + 500000LL) / 1000000LL);

            if (adjusted == 0U) {
                adjusted = out_sr;
            }
            effective_out_sr = (uint32_t) adjusted;
        }

        if (host_ch == 2U && ses_sr != 0U && out_sr != 0U) {
            size_t out_fc =
                    ((size_t) decoded_frames * (size_t) effective_out_sr + (size_t) ses_sr - 1U) / (size_t) ses_sr;
            size_t ext_need = (size_t) decoded_frames + (size_t) DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES;
            size_t wm = ext_need > out_fc ? ext_need : out_fc;
            uint64_t stream_in_before = state->pcm_src_stream_in_samples;
            uint64_t stream_out_before = state->pcm_src_stream_out_samples;
            int used_stream_soxr = 0;

            if (wm > (size_t) DASHCDG_RX_PCM_WORK_SAMPLES_MAX) {
                state->audio_decode_failures++;
                return -1;
            }

            if (ses_sr != effective_out_sr) {
                rs_tmp = (int16_t *) malloc(out_fc * 2U * sizeof(int16_t));
                if (rs_tmp == NULL) {
                    state->audio_decode_failures++;
                    return -1;
                }
#if defined(DASHCDG_HAVE_LIBSOXR)
                if (dashcdg_pcm_soxr_stream_ensure(ses_sr, effective_out_sr) &&
                        dashcdg_pcm_soxr_stream_process_interleaved(pcm, (size_t) decoded_frames, rs_tmp, out_fc)) {
                    used_stream_soxr = 1;
                    state->pcm_src_overlap_valid = 0U;
                }
#endif
                if (!used_stream_soxr) {
                    /*
                     * Safety valve: do not use overlap resampler for equal-rate trim fallback.
                     * If streaming SOXR is unavailable, stay bit-transparent for this frame.
                     */
                    if (ses_sr == out_sr && allow_sync_trim_equal_rate) {
#if defined(DASHCDG_HAVE_LIBSOXR)
                        dashcdg_pcm_soxr_stream_reset();
#endif
                        free(rs_tmp);
                        rs_tmp = NULL;
                        qptr = pcm;
                        qfc = (size_t) decoded_frames;
                        resampled_output = 0;
                        effective_out_sr = ses_sr;
                        trim_ppm = 0;
                        state->pcm_src_overlap_valid = 0U;
                    } else {
#if defined(DASHCDG_HAVE_LIBSOXR)
                        dashcdg_pcm_soxr_stream_reset();
#endif
                        dashcdg_pcm_stereo_interleaved_resample_overlap(
                                state->pcm_src_overlap_l,
                                state->pcm_src_overlap_r,
                                &state->pcm_src_overlap_valid,
                                stream_in_before,
                                stream_out_before,
                                pcm,
                                (size_t) decoded_frames,
                                ses_sr,
                                rs_tmp,
                                out_fc,
                                effective_out_sr,
                                mono_scratch,
                                wr_r_st,
                                wm
                        );
                    }
                }
                if (rs_tmp != NULL) {
                    qptr = rs_tmp;
                    qfc = out_fc;
                    resampled_output = 1;
                }
            } else {
#if defined(DASHCDG_HAVE_LIBSOXR)
                dashcdg_pcm_soxr_stream_reset();
#endif
                state->pcm_src_overlap_valid = 0U;
            }
        }

        if (resampled_output || dashcdg_v4_audio_codec_is_narrowband(codec_id)) {
            dashcdg_pcm_interleaved_s16_soft_limit_inplace((int16_t *) qptr, qfc, host_ch > 0U ? host_ch : 1U);
        }
        if (equal_rate_trim_ppm != 0 && qptr == pcm && host_ch > 0U) {
            size_t slip_qfc = qfc;
            dashcdg_rx_apply_equal_rate_trim_slip_locked(
                    state,
                    equal_rate_trim_ppm,
                    pcm,
                    &slip_qfc,
                    host_ch,
                    sizeof(pcm) / (sizeof(pcm[0]) * (size_t) host_ch)
            );
            qfc = slip_qfc;
        }

        if (effective_out_sr > 0U && qfc > 0U) {
            queued_ms_estimate = (uint32_t) (((uint64_t) qfc * 1000U + (uint64_t) effective_out_sr - 1U) /
                    (uint64_t) effective_out_sr);
        } else if (state->announced_audio_frame_ms > 0U) {
            queued_ms_estimate = (uint32_t) state->announced_audio_frame_ms;
        }
        if (wire_frame_ms > 0U) {
            queue_limit_ms += (uint32_t) wire_frame_ms * 2U;
        } else if (state->announced_audio_frame_ms > 0U) {
            queue_limit_ms += (uint32_t) state->announced_audio_frame_ms * 2U;
        }
        if (state->audio_ring_capacity_ms > 0U && queue_limit_ms > state->audio_ring_capacity_ms) {
            queue_limit_ms = state->audio_ring_capacity_ms;
        }
        if (queue_limit_ms > 0U &&
                queued_ms_estimate > 0U &&
                buffered_ms + queued_ms_estimate > queue_limit_ms) {
            if (rs_tmp != NULL) {
                free(rs_tmp);
            }
            state->audio_queue_overflows++;
            state->audio_last_queue_pressure_local_ms = dashcdg_clock_now_ms();
            return 0;
        }

        expected_queued_fc = qfc;

        dashcdg_rx_dump_pcm_to_file(qptr, qfc, host_ch);

        queued_frames = dashcdg_desktop_audio_queue_frames(
                g_audio,
                qptr,
                qfc,
                (int64_t) playback_ms
        );
        if (rs_tmp != NULL) {
            free(rs_tmp);
        }
    }
    if (queued_frames == 0U) {
        state->audio_queue_overflows++;
        state->audio_last_queue_pressure_local_ms = dashcdg_clock_now_ms();
        return 0;
    }
    state->last_audio_queue_success_local_ms = dashcdg_clock_now_ms();
    state->pcm_src_stream_in_samples += (uint64_t) decoded_frames;
    state->pcm_src_stream_out_samples += (uint64_t) queued_frames;
    /*
     * PortAudio may accept a prefix under back-pressure; compare against the frame count we offered
     * (after optional resampling). Partial accept still advances jitter so we do not wedge.
     */
    if (queued_frames != expected_queued_fc) {
        state->audio_queue_overflows++;
        state->audio_last_queue_pressure_local_ms = dashcdg_clock_now_ms();
    }

    return 1;
}

static void dashcdg_rx_apply_equal_rate_trim_slip_locked(
        struct receiver_state *state,
        int32_t trim_ppm,
        int16_t *interleaved_pcm,
        size_t *inout_frames,
        unsigned int channels,
        size_t frame_capacity
) {
    int64_t accum;

    if (state == NULL || interleaved_pcm == NULL || inout_frames == NULL || channels == 0U || frame_capacity == 0U || trim_ppm == 0) {
        return;
    }
    accum = state->audio_equal_rate_slip_accum + (int64_t) trim_ppm * (int64_t) (*inout_frames);
    while (accum >= 1000000LL) {
        if (*inout_frames >= frame_capacity) {
            break;
        }
        {
            size_t src = (*inout_frames - 1U) * channels;
            size_t dst = (*inout_frames) * channels;
            for (unsigned int ch = 0U; ch < channels; ++ch) {
                interleaved_pcm[dst + ch] = interleaved_pcm[src + ch];
            }
        }
        (*inout_frames)++;
        accum -= 1000000LL;
    }
    while (accum <= -1000000LL) {
        if (*inout_frames > 1U) {
            (*inout_frames)--;
        }
        accum += 1000000LL;
    }
    state->audio_equal_rate_slip_accum = accum;
}

static int dashcdg_rx_apply_audio_frame_locked(
        struct receiver_state *state,
        const struct dashcdg_audio_jitter_frame *frame
) {
    static uint64_t s_diag_last_ms = 0U;
    static uint64_t s_diag_frames = 0U;
    static uint64_t s_diag_near_silent_frames = 0U;
    static uint64_t s_diag_encoded_bytes = 0U;
    static uint64_t s_diag_peak_accum = 0U;
    static uint32_t s_diag_peak_max = 0U;
    int16_t pcm[DASHCDG_RX_PCM_INTERLEAVED_SAMPLES_MAX];
    int16_t mono_scratch[DASHCDG_RX_PCM_WORK_SAMPLES_MAX];
    uint16_t host_output_channels;
    int decoded_frames;
    int32_t trim_ppm_for_frame = 0;
    int have_trim_ppm_for_frame = 0;
    uint64_t local_now_ms;

    if (state == NULL || frame == NULL || !state->network_audio_enabled || g_audio_decode_disabled || g_audio == NULL) {
        return 0;
    }
    host_output_channels = dashcdg_rx_portaudio_output_channels(state->announced_audio_channels);

    /*
     * Steady state should hold near the target playout buffer, not hammer the queue path until we
     * hit the hard ring limit. Retrying every tick at 340/350 ms produced endless "overflow" noise
     * and audible break-up even though the device was already at the intended latency.
     */
    if (g_audio_stream_started && dashcdg_desktop_audio_output_device_ready(g_audio)) {
        local_now_ms = dashcdg_clock_now_ms();
        uint32_t buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
        uint32_t target_ms = dashcdg_rx_audio_target_buffer_ms_locked(state);
        uint32_t high_water_ms = target_ms;
        uint8_t frame_ms = frame->frame_ms > 0 ? frame->frame_ms : state->announced_audio_frame_ms;

        if (frame_ms > 0U) {
            high_water_ms += (uint32_t) frame_ms;
        }
        if (state->audio_last_servo_update_local_ms == 0U ||
                local_now_ms - state->audio_last_servo_update_local_ms >= DASHCDG_RX_QUEUE_SERVO_HOLD_UPDATE_MS ||
                buffered_ms < high_water_ms) {
            trim_ppm_for_frame = dashcdg_rx_audio_resample_trim_ppm_locked(state, buffered_ms);
            have_trim_ppm_for_frame = 1;
        }
        if (target_ms > 0U && buffered_ms >= high_water_ms) {
            return 0;
        }
    }

    if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
        if (g_opus_decoder.channels == 1) {
            decoded_frames = dashcdg_opus_decode_frame(
                    &g_opus_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0 && host_output_channels == 2U) {
                if ((size_t) decoded_frames * 2U > sizeof(pcm) / sizeof(pcm[0])) {
                    state->audio_decode_failures++;
                    return -1;
                }
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            } else if (decoded_frames > 0) {
                memcpy(pcm, mono_scratch, (size_t) decoded_frames * sizeof(int16_t));
            }
        } else {
            decoded_frames = dashcdg_opus_decode_frame(
                    &g_opus_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
#else
        decoded_frames = 0;
#endif
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        if (g_amr_wb_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_amr_wb_decoder_run(
                    g_amr_wb_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0) {
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            }
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        if (g_amr_nb_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_amr_nb_decoder_run(
                    g_amr_nb_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0) {
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            }
        }
    } else if (dashcdg_v4_audio_codec_is_nb_ima_payload(frame->codec_id)) {
        decoded_frames = dashcdg_nb_ima_decode_to_pcm48_mono_frame(
                &g_nb_ima_decoder,
                frame->encoded_bytes,
                frame->encoded_length,
                mono_scratch,
                sizeof(mono_scratch) / sizeof(mono_scratch[0])
        );
        if (decoded_frames > 0) {
            dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                    mono_scratch,
                    pcm,
                    (size_t) decoded_frames
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_QCELP8K) {
        if (g_evrc_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_qcelp8k_decode_to_pcm48_stereo(
                    g_evrc_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
        if (g_qcelp_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_qcelp13k_decode_to_pcm48_stereo(
                    g_qcelp_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
        if (g_bt_sbc_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_bt_sbc_decode_to_pcm48_stereo(
                    g_bt_sbc_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else {
        decoded_frames = 0;
    }
    if (decoded_frames <= 0) {
        state->audio_decode_failures++;
        return -1;
    }
    if ((size_t) decoded_frames * (size_t) (host_output_channels == 0U ? 1U : host_output_channels) >
            sizeof(pcm) / sizeof(pcm[0])) {
        state->audio_decode_failures++;
        return -1;
    }

    {
        uint32_t peak = dashcdg_rx_pcm_peak_abs_s16(
                pcm,
                (size_t) decoded_frames * (size_t) (host_output_channels == 0U ? 1U : host_output_channels)
        );
        uint64_t now_ms = dashcdg_clock_now_ms();

        s_diag_frames++;
        s_diag_encoded_bytes += (uint64_t) frame->encoded_length;
        s_diag_peak_accum += (uint64_t) peak;
        if (peak > s_diag_peak_max) {
            s_diag_peak_max = peak;
        }
        if (peak < 64U) {
            s_diag_near_silent_frames++;
        }
        if (s_diag_last_ms == 0U) {
            s_diag_last_ms = now_ms;
        } else if (now_ms > s_diag_last_ms && now_ms - s_diag_last_ms >= 2000U) {
            char line[256];
            uint32_t avg_peak = s_diag_frames > 0U ? (uint32_t) (s_diag_peak_accum / s_diag_frames) : 0U;
            uint32_t silent_pct_x100 = s_diag_frames > 0U
                    ? (uint32_t) ((s_diag_near_silent_frames * 10000ULL) / s_diag_frames)
                    : 0U;

            snprintf(
                    line,
                    sizeof(line),
                    "[rx] audio-signal codec=%u frames=%" DASHCDG_RX_PRIu64 " encB=%" DASHCDG_RX_PRIu64 " peak_avg=%u peak_max=%u near_silent=%u.%02u%%",
                    (unsigned int) frame->codec_id,
                    (uint64_t) s_diag_frames,
                    (uint64_t) s_diag_encoded_bytes,
                    (unsigned int) avg_peak,
                    (unsigned int) s_diag_peak_max,
                    (unsigned int) (silent_pct_x100 / 100U),
                    (unsigned int) (silent_pct_x100 % 100U)
            );
            line[sizeof(line) - 1U] = '\0';
            dashcdg_rx_async_stdout_line(line);
            s_diag_last_ms = now_ms;
            s_diag_frames = 0U;
            s_diag_near_silent_frames = 0U;
            s_diag_encoded_bytes = 0U;
            s_diag_peak_accum = 0U;
            s_diag_peak_max = 0U;
        }
    }

    return dashcdg_rx_queue_decoded_interleaved_pcm_locked(
            state,
            decoded_frames,
            pcm,
            mono_scratch,
            host_output_channels,
            trim_ppm_for_frame,
            have_trim_ppm_for_frame,
            frame->playback_ms,
            frame->frame_ms,
            frame->codec_id);
}

static int dashcdg_rx_apply_amr_wb_lost_skips_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        uint64_t sender_playback_now_ms,
        int have_sender_playback,
        uint64_t miss_delta
) {
    int16_t pcm[DASHCDG_RX_PCM_INTERLEAVED_SAMPLES_MAX];
    int16_t mono_scratch[DASHCDG_RX_PCM_WORK_SAMPLES_MAX];
    uint16_t host_output_channels;
    uint64_t skip;
    uint8_t frame_ms_line;
    int32_t trim_ppm_for_frame = 0;
    int have_trim_ppm_for_frame = 0;

    if (state == NULL || state->announced_audio_codec_id != DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        return 1;
    }
    if (g_amr_wb_decoder == NULL) {
        dashcdg_amr_wb_decoder_create(&g_amr_wb_decoder);
    }
    if (g_amr_wb_decoder == NULL) {
        return 0;
    }

    skip = miss_delta > 0U ? miss_delta : 1U;
    if (skip > 64U) {
        skip = 64U;
    }

    for (; skip > 0U; skip--) {
        int ln;
        int qrc;

        ln = dashcdg_amr_wb_decoder_run_lost(g_amr_wb_decoder, mono_scratch, 960U);
        if (ln <= 0) {
            break;
        }

        if (!state->network_audio_enabled || g_audio_decode_disabled || g_audio == NULL) {
            continue;
        }

        host_output_channels = dashcdg_rx_portaudio_output_channels(state->announced_audio_channels);
        frame_ms_line = state->announced_audio_frame_ms;

        if (g_audio_stream_started && dashcdg_desktop_audio_output_device_ready(g_audio)) {
            uint32_t buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
            uint32_t target_ms = dashcdg_rx_audio_target_buffer_ms_locked(state);
            uint32_t high_water_ms = target_ms;

            if (frame_ms_line > 0U) {
                high_water_ms += (uint32_t) frame_ms_line;
            }
            if (state->audio_last_servo_update_local_ms == 0U ||
                    local_now_ms - state->audio_last_servo_update_local_ms >= DASHCDG_RX_QUEUE_SERVO_HOLD_UPDATE_MS ||
                    buffered_ms < high_water_ms) {
                trim_ppm_for_frame = dashcdg_rx_audio_resample_trim_ppm_locked(state, buffered_ms);
                have_trim_ppm_for_frame = 1;
            }
            if (target_ms > 0U && buffered_ms >= high_water_ms) {
                break;
            }
        }

        if (host_output_channels == 2U) {
            dashcdg_rx_pcm48_mono_to_interleaved_stereo(mono_scratch, pcm, (size_t) ln);
        } else {
            memcpy(pcm, mono_scratch, (size_t) ln * sizeof(int16_t));
        }

        qrc = dashcdg_rx_queue_decoded_interleaved_pcm_locked(
                state,
                ln,
                pcm,
                mono_scratch,
                host_output_channels,
                trim_ppm_for_frame,
                have_trim_ppm_for_frame,
                have_sender_playback ? sender_playback_now_ms : 0ULL,
                frame_ms_line,
                DASHCDG_V4_AUDIO_CODEC_AMR_WB);
        if (qrc <= 0) {
            break;
        }
    }

    return 1;
}

static void dashcdg_rx_apply_cdg_batch_locked(
        struct receiver_state *state,
        const struct dashcdg_cdg_batch_jitter_frame *batch,
        uint64_t local_now_ms
) {
    const struct dashcdg_subchannel_packet *packets;

    if (state == NULL || batch == NULL || batch->packet_count == 0) {
        return;
    }

    dashcdg_rx_seed_live_state_before_first_wire_delta_locked(state, local_now_ms);

    packets = (const struct dashcdg_subchannel_packet *) batch->packet_bytes;
    for (uint8_t i = 0; i < batch->packet_count; ++i) {
        dashcdg_cdg_state_process_packet(&state->live_state, &packets[i]);
        state->live_packets_applied++;
    }
    state->v4_bridge_cdg_valid = 0;
    state->last_progress_local_ms = local_now_ms;
}

static void dashcdg_rx_drain_media_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t sender_playback_now_ms = 0U;
    int have_sender_playback = 0;
    uint64_t local_audio_playback_now_ms = 0U;
    int have_local_audio_playback = 0;
    size_t combined_steps = 0U;

    if (state == NULL) {
        return;
    }

    if (dashcdg_rx_source_idle_and_drained_locked(state, local_now_ms)) {
        return;
    }

    have_sender_playback = dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_now_ms);
    have_local_audio_playback = dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_now_ms);

    /*
     * Drain audio before CDG: if the PCM ring is full we must not apply CDG for the same
     * timeline, or graphics lead heard audio by the entire buffer depth.
     */
    while (combined_steps < DASHCDG_MAX_DRAIN_STEPS_PER_CALL * 2U) {
        struct dashcdg_audio_jitter_frame *frame = NULL;
        uint64_t miss_delta = 0U;
        struct dashcdg_audio_jitter_drain_input din;
        enum dashcdg_audio_drain_step step;
        struct dashcdg_cdg_batch_jitter_frame *batch = NULL;
        struct dashcdg_cdg_batch_jitter_drain_input cdg_din;
        enum dashcdg_cdg_batch_drain_step cdg_step;
        uint64_t cdg_miss = 0U;
        int progressed = 0;

        if (!g_audio_decode_disabled && state->audio_jitter.initialized) {
            int audio_skip_hold_active = 0;
            int audio_backpressure_hold_active = 0;

            memset(&din, 0, sizeof(din));
            din.have_sender_playback = have_sender_playback;
            din.sender_playback_now_ms = sender_playback_now_ms;
            din.announced_audio_frame_ms = state->announced_audio_frame_ms;
            din.announced_playout_delay_ms = state->announced_playout_delay_ms;
            din.late_grace_ms = DASHCDG_AUDIO_LATE_GRACE_MS;
            din.audio_stream_started = g_audio_stream_started;
            din.audio_device_null = g_audio == NULL ? 1 : 0;
            din.audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
            if (g_audio != NULL) {
                uint32_t host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();

                /*
                 * Skip gating must account for host/device output buffering as well as app-ring
                 * depth. Using app-ring alone can classify continuity loss too early on hosts with
                 * larger output latency and produce repetitive audio_continuity_skip spam.
                 */
                if (din.audio_buffered_ms > UINT32_MAX - host_latency_ms) {
                    din.audio_buffered_ms = UINT32_MAX;
                } else {
                    din.audio_buffered_ms += host_latency_ms;
                }
            }
            din.ms_since_prior_audio_apply = 0U;
            if (state->last_audio_jitter_apply_local_ms != 0U) {
                din.ms_since_prior_audio_apply = dashcdg_rx_elapsed_ms_safe(
                        local_now_ms,
                        state->last_audio_jitter_apply_local_ms
                );
            }
            din.primed_decode = state->jitter_audio_decode_primed;
            audio_skip_hold_active = state->audio_skip_hold_until_local_ms != 0U &&
                    local_now_ms < state->audio_skip_hold_until_local_ms;
            if (audio_skip_hold_active) {
                din.ms_since_prior_audio_apply = 0U;
                din.primed_decode = 0;
            }
            audio_backpressure_hold_active = dashcdg_rx_audio_backpressure_hold_active_locked(state, local_now_ms);

            if (!audio_backpressure_hold_active) {
                step = dashcdg_audio_jitter_drain_step(&state->audio_jitter, &din, &frame, &miss_delta);
                if (step == DASHCDG_AUDIO_DRAIN_SKIP) {
                    state->audio_missing_skips += miss_delta;
                    if (miss_delta >= 8U) {
                        state->audio_hard_resync_events++;
                    }
                    if (miss_delta >= 8U) {
                        if (state->last_audio_hard_resync_local_ms == 0U ||
                                local_now_ms <= state->last_audio_hard_resync_local_ms ||
                                local_now_ms - state->last_audio_hard_resync_local_ms >=
                                        DASHCDG_RX_AUDIO_HARD_RESYNC_LOG_COOLDOWN_MS) {
                            RX_OUT(
                                    "[rx] audio hard-resync: skipped=%" DASHCDG_RX_PRIu64 " next_seq=%u pending=%u\n",
                                    (uint64_t) miss_delta,
                                    (unsigned int) state->audio_jitter.next_media_sequence,
                                    (unsigned int) dashcdg_audio_jitter_occupied_count(&state->audio_jitter)
                            );
                            state->last_audio_hard_resync_local_ms = local_now_ms;
                        }
                    }
                    (void) dashcdg_rx_apply_amr_wb_lost_skips_locked(
                            state,
                            local_now_ms,
                            sender_playback_now_ms,
                            have_sender_playback,
                            miss_delta
                    );
                    state->jitter_audio_decode_primed = 1;
                    state->last_audio_jitter_apply_local_ms = local_now_ms;
                    state->last_progress_local_ms = local_now_ms;
                    progressed = 1;
                } else if (step == DASHCDG_AUDIO_DRAIN_APPLY && frame != NULL) {
                    uint8_t frame_ms = frame->frame_ms > 0 ? frame->frame_ms : state->announced_audio_frame_ms;
                    int apply_rc = dashcdg_rx_apply_audio_frame_locked(state, frame);

                    if (apply_rc == 0) {
                        /* Device buffer full: keep the jitter slot; CDG still drains in the same tick. */
                    } else if (apply_rc < 0) {
                        /*
                         * A decoder failure means this slot is unusable, not backpressure. Drop it so a
                         * bad codec hop/frame cannot pin next_media_sequence forever, but do not mark the
                         * jitter session as decode-primed from a failed frame.
                         */
                        dashcdg_audio_jitter_note_applied(&state->audio_jitter, frame, frame_ms);
                        state->last_audio_jitter_apply_local_ms = local_now_ms;
                        state->last_progress_local_ms = local_now_ms;
                        progressed = 1;
                    } else {
                        dashcdg_audio_jitter_note_applied(&state->audio_jitter, frame, frame_ms);
                        state->jitter_audio_decode_primed = 1;
                        state->last_audio_jitter_apply_local_ms = local_now_ms;
                        state->last_progress_local_ms = local_now_ms;
                        progressed = 1;
                    }
                }
            }
        }

        /*
         * CDG drain must run even when the PCM ring is full (apply_rc==0 / backpressure). Otherwise the
         * inner loop exits with progressed=0 before any CDG step — WinMM/slow hosts keep the ring
         * pegged and CDG never advances (late join: audio ok, render_gate stuck on v4-startup/black).
         * Graphics timing still follows playback_ms / DAC clock from publish_render_snapshot.
         */
        if (state->cdg_batch_jitter.initialized) {
            int cdg_skip_hold_active = 0;

            memset(&cdg_din, 0, sizeof(cdg_din));
            if (have_local_audio_playback) {
                cdg_din.have_sender_playback = 1;
                cdg_din.sender_playback_now_ms = local_audio_playback_now_ms;
                cdg_din.announced_playout_delay_ms = 0U;
            } else {
                cdg_din.have_sender_playback = have_sender_playback;
                cdg_din.sender_playback_now_ms = sender_playback_now_ms;
                cdg_din.announced_playout_delay_ms = state->announced_playout_delay_ms;
            }
            cdg_din.late_grace_ms = DASHCDG_CDG_LATE_GRACE_MS;
            cdg_din.late_gate =
                    (state->cdg_snapshots_applied > 0U || state->live_packets_applied > 0U ||
                            dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter) > 0U)
                            ? 1
                            : 0;
            cdg_din.ms_since_prior_cdg_apply = 0U;
            if (state->last_cdg_jitter_apply_local_ms != 0U) {
                cdg_din.ms_since_prior_cdg_apply = dashcdg_rx_elapsed_ms_safe(
                        local_now_ms,
                        state->last_cdg_jitter_apply_local_ms
                );
            }
            cdg_din.primed_decode = state->jitter_cdg_decode_primed;
            cdg_skip_hold_active = state->cdg_skip_hold_until_local_ms != 0U &&
                    local_now_ms < state->cdg_skip_hold_until_local_ms;
            if (cdg_skip_hold_active) {
                cdg_din.late_gate = 0;
                cdg_din.ms_since_prior_cdg_apply = 0U;
                cdg_din.primed_decode = 0;
            }

            cdg_step = dashcdg_cdg_batch_jitter_drain_step(&state->cdg_batch_jitter, &cdg_din, &batch, &cdg_miss);
            if (cdg_step == DASHCDG_CDG_BATCH_DRAIN_SKIP) {
                state->live_missing_skips += cdg_miss;
                state->last_cdg_jitter_apply_local_ms = local_now_ms;
                state->last_progress_local_ms = local_now_ms;
                progressed = 1;
            } else if (cdg_step == DASHCDG_CDG_BATCH_DRAIN_APPLY && batch != NULL) {
                dashcdg_rx_apply_cdg_batch_locked(state, batch, local_now_ms);
                dashcdg_cdg_batch_jitter_note_applied(&state->cdg_batch_jitter, batch);
                state->jitter_cdg_decode_primed = 1;
                state->last_cdg_jitter_apply_local_ms = local_now_ms;
                progressed = 1;
            }
        }

        if (!progressed) {
            break;
        }
        combined_steps++;
    }

    dashcdg_rx_purge_audio_fec_locked(state);
    dashcdg_rx_purge_cdg_fec_locked(state);
}

static void dashcdg_rx_print_status_locked(void) {
    uint32_t prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t stall_ms = 0;
    uint64_t since_last_dg_ms = 0;
    uint64_t clock_hold_ms = 0;
    uint32_t audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    uint32_t audio_target_buffer_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    uint32_t audio_host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    uint32_t audio_target_total_ms = dashcdg_rx_audio_target_total_latency_ms_locked(&g_receiver);
    size_t pending_audio = dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter);
    size_t pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    size_t tracked_audio_groups = 0;
    size_t tracked_cdg_groups = 0;
    size_t audio_groups_with_parity = 0;
    size_t cdg_groups_with_parity = 0;
    size_t audio_repairable = 0;
    size_t cdg_repairable = 0;
    char audio_gate[64];
    char render_gate[64];
    int muted = g_audio != NULL ? dashcdg_desktop_audio_is_muted(g_audio) : g_audio_muted;

    stall_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_progress_local_ms);
    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_datagram_local_ms);
    clock_hold_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_clock_update_local_ms);

    dashcdg_rx_collect_fec_group_stats(
            g_receiver.audio_fec_groups,
            &tracked_audio_groups,
            &audio_groups_with_parity,
            &audio_repairable
    );
    dashcdg_rx_collect_fec_group_stats(
            g_receiver.cdg_fec_groups,
            &tracked_cdg_groups,
            &cdg_groups_with_parity,
            &cdg_repairable
    );
    dashcdg_rx_format_audio_gate_locked(&g_receiver, now_ms, audio_gate, sizeof(audio_gate));
    dashcdg_rx_format_render_gate_locked(&g_receiver, render_gate, sizeof(render_gate));
    (void) since_last_dg_ms;
    (void) clock_hold_ms;
    (void) tracked_audio_groups;
    (void) tracked_cdg_groups;
    (void) audio_groups_with_parity;
    (void) cdg_groups_with_parity;
    (void) audio_repairable;
    (void) cdg_repairable;
    (void) muted;

    RX_OUT(
            "[rx] net dg=%" DASHCDG_RX_PRIu64 " parse=%" DASHCDG_RX_PRIu64 " a/v4=%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
            " rwin=%" DASHCDG_RX_PRIu64 " unk=%" DASHCDG_RX_PRIu64 " asset=%u/%u chunks=%u/%u\n",
            (unsigned long long) g_receiver.datagrams_received,
            (unsigned long long) g_receiver.parse_failures,
            (unsigned long long) g_receiver.v4_audio_chunk_packets,
            (unsigned long long) g_receiver.v4_video_delta_packets,
            (unsigned long long) g_receiver.v4_repair_window_packets,
            (unsigned long long) g_receiver.unknown_packets,
            (unsigned int) prefix_bytes,
            (unsigned int) g_receiver.asset_size,
            (unsigned int) g_receiver.received_chunks,
            (unsigned int) g_receiver.chunk_count
    );
    RX_OUT(
            "[rx] audio buf=%u/%u+%u=%u pend=%u/%u rec=%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
            " fail=%" DASHCDG_RX_PRIu64 " gate=%s render=%s ready=%d clk=%d pause=%d stall=%" DASHCDG_RX_PRIu64 "ms\n",
            (unsigned int) audio_buffered_ms,
            (unsigned int) audio_target_buffer_ms,
            (unsigned int) audio_host_latency_ms,
            (unsigned int) audio_target_total_ms,
            (unsigned int) pending_audio,
            (unsigned int) pending_cdg,
            (unsigned long long) g_receiver.fec_audio_recovered,
            (unsigned long long) g_receiver.fec_cdg_recovered,
            (unsigned long long) g_receiver.fec_recovery_failures,
            audio_gate,
            render_gate,
            g_receiver.reader_ready,
            g_receiver.have_clock,
            g_receiver.playback_paused,
            (unsigned long long) stall_ms
    );
    dashcdg_rx_sidecar_write_line("[rx] status emitted; see console for full live line");
}

static void handle_live_cdg_batch(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_store_cdg_batch_locked(state, view);
}

static void handle_audio_frame(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || !state->network_audio_enabled || g_audio_decode_disabled) {
        return;
    }

    dashcdg_rx_store_audio_frame_locked(state, view);
}

static void send_ptp_delay_request(
        struct receiver_state *state,
        dashcdg_socket_t sockfd,
        const struct sockaddr_in *destination,
        uint64_t local_now_ms
) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header header;
    struct dashcdg_ptp_delay_req_payload payload;
    size_t packet_size;

    if (state == NULL || destination == NULL || sockfd == DASHCDG_INVALID_SOCKET) {
        return;
    }

    memset(&header, 0, sizeof(header));
    memset(&payload, 0, sizeof(payload));
    header.sequence = (uint32_t) (state->datagrams_received + state->ptp_delay_req_packets + 1U);
    header.sender_time_ms = local_now_ms;
    payload.request_id = state->next_delay_request_id++;

    packet_size = dashcdg_protocol_serialize_ptp_delay_req(packet, sizeof(packet), &header, &payload);
    if (packet_size == 0) {
        return;
    }
    if (sendto(sockfd, (const char *) packet, (int) packet_size, 0, (const struct sockaddr *) destination, sizeof(*destination)) !=
            (int) packet_size) {
        return;
    }

    state->ptp_delay_req_packets++;
    state->pending_delay_request_id = payload.request_id;
    state->pending_delay_request_local_ms = local_now_ms;
    state->pending_delay_request_valid = 1;
}

/*
 * Software PCM ring sizing for network streaming. Keep enough slack for jitter + OS output
 * buffering without multi-second queues (which made CDG/graphics lead heard audio).
 */
static uint32_t dashcdg_rx_network_stream_ring_ms(uint16_t playout_delay_ms, uint8_t codec_id) {
    uint32_t total_target_ms;
    uint32_t host_ms;
    uint32_t target_buffer_ms;
    uint32_t ring_headroom_ms;

    (void) codec_id;

    total_target_ms = playout_delay_ms > 0U ? (uint32_t) playout_delay_ms : DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
    host_ms = dashcdg_rx_audio_host_latency_ms_locked();
    if (total_target_ms > host_ms + DASHCDG_RX_APP_RING_SAFETY_MS) {
        target_buffer_ms = total_target_ms - host_ms - DASHCDG_RX_APP_RING_SAFETY_MS;
    } else {
        target_buffer_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (target_buffer_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_APP_RING_TARGET_MS;
    }
    ring_headroom_ms = DASHCDG_RX_APP_RING_HEADROOM_MS;
    if (total_target_ms > target_buffer_ms) {
        uint32_t runway_gap_ms = total_target_ms - target_buffer_ms;

        if (runway_gap_ms > ring_headroom_ms) {
            ring_headroom_ms = runway_gap_ms;
        }
    }
    target_buffer_ms += ring_headroom_ms;
    if (target_buffer_ms < DASHCDG_RX_MIN_RING_CAPACITY_MS) {
        target_buffer_ms = DASHCDG_RX_MIN_RING_CAPACITY_MS;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_RING_CAPACITY_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_RING_CAPACITY_MS;
    }
    return target_buffer_ms;
}

static void handle_announce(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int song_changed = strcmp(state->song_id, view->announce.song_id) != 0;
    int session_changed = state->session_start_ms != 0 && state->session_start_ms != view->announce.session_start_ms;
    /*
     * Same edge as v4_session_info: session_changed is false when state->session_start_ms is 0, so the
     * first announce adopting a non-zero session must still reset jitter/CDG state (CDG-only or v3).
     */
    int cold_session_adopt =
            state->session_start_ms == 0U && view->announce.session_start_ms != 0U;
    int asset_changed = state->asset_size != view->announce.asset_size ||
            state->chunk_size != (view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size);
    int has_network_audio = view->announce.audio_sample_rate > 0 && view->announce.audio_channels > 0 && view->announce.audio_frame_ms > 0;

    if (song_changed || session_changed || cold_session_adopt) {
        receiver_state_reset(state);
    }

    if (view->announce.asset_size > 0 && !receiver_state_prepare_asset(
            state,
            view->announce.asset_size,
            view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size
    )) {
        return;
    }
    strncpy(state->song_id, view->announce.song_id, sizeof(state->song_id) - 1U);
    state->session_start_ms = view->announce.session_start_ms;
    state->announced_audio_sample_rate = view->announce.audio_sample_rate;
    state->announced_audio_channels = view->announce.audio_channels;
    state->announced_playout_delay_ms = view->announce.playout_delay_ms;
    state->announced_audio_frame_ms = view->announce.audio_frame_ms;
    state->announced_transport_version = DASHCDG_PROTOCOL_VERSION;
    state->announced_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    state->announced_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
    state->announced_audio_fec_group_size = view->announce.audio_fec_group_size;
    state->announced_cdg_fec_group_size = view->announce.cdg_fec_group_size;
    state->network_audio_enabled = has_network_audio;
    if (song_changed || session_changed || asset_changed || !state->have_clock) {
        dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
        state->have_clock = 1;
        dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    }

    if ((song_changed || session_changed || asset_changed || cold_session_adopt) && has_network_audio && !g_audio_decode_disabled) {
        if (g_audio == NULL) {
            g_audio = dashcdg_desktop_audio_new();
        }
        if (g_audio != NULL) {
            dashcdg_desktop_audio_stop_stream(g_audio);
            state->pcm_src_overlap_valid = 0U;
            state->pcm_src_stream_in_samples = 0U;
            state->pcm_src_stream_out_samples = 0U;
#if defined(DASHCDG_HAVE_LIBSOXR)
            dashcdg_pcm_soxr_stream_reset();
#endif
            state->audio_resample_trim_ppm = 0;
            state->last_audio_queue_success_local_ms = 0U;
            state->last_audio_timestamp_advance_local_ms = 0U;
            state->last_audio_timestamp_ms = -1;
            state->last_observed_stream_underrun_events = g_audio->stream_underrun_events;
            state->last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
            if (!dashcdg_desktop_audio_init_stream(
                        g_audio,
                        view->announce.audio_sample_rate,
                        dashcdg_rx_portaudio_output_channels(view->announce.audio_channels),
                        dashcdg_rx_network_stream_ring_ms(view->announce.playout_delay_ms, DASHCDG_V4_AUDIO_CODEC_OPUS)
                )) {
                RX_ERR( "[rx] announce: desktop_audio_init_stream failed\n");
                g_audio_stream_started = 0;
                g_audio_start_inflight = 0;
                state->rx_audio_applied_valid = 0;
                dashcdg_opus_decoder_free(&g_opus_decoder);
                dashcdg_rx_amr_decoders_release();
            } else {
                dashcdg_opus_decoder_free(&g_opus_decoder);
                dashcdg_rx_amr_decoders_release();
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
                dashcdg_opus_decoder_init(
                        &g_opus_decoder,
                        view->announce.audio_sample_rate,
                        view->announce.audio_channels,
                        view->announce.audio_frame_ms
                );
#endif
                dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
                g_audio_stream_started = 0;
                g_audio_start_inflight = 0;
            }
        }
    }

    if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
    }

    if (song_changed || session_changed || asset_changed || cold_session_adopt) {
        RX_OUT( "[rx] announced %s (%u bytes)\n", state->song_id, view->announce.asset_size);
    }
}

static void dashcdg_rx_configure_audio_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        uint16_t sample_rate,
        uint8_t channels,
        uint8_t frame_ms,
        uint16_t playout_delay_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id
) {
    uint32_t buffer_ms;
    uint16_t host_ch;

    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled) {
        return;
    }

    /*
     * Cold reopen after the stream was torn down (!rx_audio_applied_valid) must not reuse an old
     * playback anchor: sender_playback_now vs queued frame playback_ms wedges claim_audio_start and
     * startup sounds wrong until session_start/track change clears bases via receiver_state_reset.
     * If v4 audio arrived before the first successful configure, keep bases we already bootstrapped
     * from the first chunk (see store_v4_audio_frame_locked).
     */
    if (!state->rx_audio_applied_valid &&
            !(
                    dashcdg_audio_jitter_occupied_count(&state->audio_jitter) > 0U &&
                    state->playback_base_sender_ms != 0U
            )) {
        state->playback_base_ms = 0U;
        state->playback_base_sender_ms = 0U;
    }

    if (g_audio == NULL) {
        g_audio = dashcdg_desktop_audio_new();
    }
    if (g_audio == NULL) {
        return;
    }

    buffer_ms = dashcdg_rx_network_stream_ring_ms(playout_delay_ms, codec_id);
    dashcdg_rx_refresh_audio_latency_budget_locked(state, playout_delay_ms, frame_ms, buffer_ms);
    host_ch = dashcdg_rx_portaudio_output_channels((uint8_t) channels);

    dashcdg_desktop_audio_stop_stream(g_audio);
#if DASHCDG_HAVE_PORTAUDIO
    g_rx_pa_stream_inactive_streak = 0U;
#endif
    g_rx_force_full_preroll_start = 1;
    state->pcm_src_overlap_valid = 0;
    state->pcm_src_stream_in_samples = 0;
    state->pcm_src_stream_out_samples = 0;
#if defined(DASHCDG_HAVE_LIBSOXR)
    dashcdg_pcm_soxr_stream_reset();
#endif
    state->last_audio_queue_success_local_ms = 0U;
    state->last_audio_timestamp_advance_local_ms = 0U;
    state->last_audio_timestamp_ms = -1;
    state->audio_resample_trim_ppm = 0;
    state->audio_servo_enable_after_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_audio_servo_warmup_ms_locked(state)
    );
    /*
     * Track/session reopen is not backpressure; keep servo responsive so peers converge quickly
     * after next-track and TX-restart transitions.
     */
    state->audio_last_queue_pressure_local_ms = 0U;
    if (!dashcdg_desktop_audio_init_stream(
                g_audio,
                sample_rate,
                host_ch,
                buffer_ms
        )) {
        RX_ERR(
                "[rx] audio: init_stream failed (sr=%u host_ch=%u buf_ms=%u)\n",
                (unsigned int) sample_rate,
                (unsigned int) host_ch,
                (unsigned int) buffer_ms
        );
        state->rx_audio_applied_valid = 0;
        return;
    }
    if (g_audio->stream_sample_rate > 0U) {
        state->audio_ring_capacity_ms =
                (uint32_t) ((g_audio->stream_capacity_frames * 1000U) / g_audio->stream_sample_rate);
    }
    dashcdg_rx_refresh_audio_latency_budget_locked(state, playout_delay_ms, frame_ms, state->audio_ring_capacity_ms);

    /*
     * Drop queued frames and FEC trackers only after the ring is allocated so a transient OOM in
     * init_stream does not wipe jitter while leaving decoders stale. If v4 audio already filled the
     * reorder buffer before the first session_info-driven configure (late-join / reorder), do not
     * clear it when the wire parameters match the last successful setup (or the pre-configure path
     * with !rx_audio_applied_valid but pending frames).
     */
    {
        int keep_encoded_jitter = 0;

        if (dashcdg_audio_jitter_occupied_count(&state->audio_jitter) > 0U) {
            if (state->rx_audio_applied_valid) {
                keep_encoded_jitter = sample_rate == state->rx_audio_applied_wire_sr &&
                        channels == state->rx_audio_applied_wire_ch &&
                        frame_ms == state->rx_audio_applied_frame_ms &&
                        playout_delay_ms == state->rx_audio_applied_preroll_ms &&
                        audio_profile_id == state->rx_audio_applied_profile_id &&
                        codec_id == state->rx_audio_applied_codec_id;
            } else {
                keep_encoded_jitter = 1;
            }
        }
        if (!keep_encoded_jitter) {
            dashcdg_audio_jitter_clear(&state->audio_jitter);
            memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
            state->jitter_audio_decode_primed = 0;
            dashcdg_opus_decoder_free(&g_opus_decoder);
            dashcdg_rx_amr_decoders_release();
        } else {
            memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
        }
    }
    if (!dashcdg_rx_init_audio_decoder_for_codec(codec_id, sample_rate, channels, frame_ms)) {
        RX_ERR(
                "[rx] audio: failed to initialize decoder for codec=%u sr=%u ch=%u frame_ms=%u\n",
                (unsigned int) codec_id,
                (unsigned int) sample_rate,
                (unsigned int) channels,
                (unsigned int) frame_ms
        );
    }
    dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    state->audio_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_startup_skip_hold_ms(playout_delay_ms, state->rx_audio_applied_valid)
    );

    RX_OUT(
            "[rx] audio: output ring (session_sr=%u pa_open_request_hz=%u wire_ch=%u host_ch=%u frame_ms=%u preroll=%u"
            " target_total=%u host=%u target_buf=%u ring=%u prof=%u codec=%u)\n",
            (unsigned int) sample_rate,
            (unsigned int) g_audio->stream_sample_rate,
            (unsigned int) channels,
            (unsigned int) host_ch,
            (unsigned int) frame_ms,
            (unsigned int) playout_delay_ms,
            (unsigned int) state->audio_target_total_latency_ms,
            (unsigned int) state->audio_host_output_latency_ms,
            (unsigned int) state->audio_target_buffer_ms,
            (unsigned int) state->audio_ring_capacity_ms,
            (unsigned int) audio_profile_id,
            (unsigned int) codec_id
    );

    state->rx_audio_applied_wire_sr = sample_rate;
    state->rx_audio_applied_wire_ch = channels;
    state->rx_audio_applied_frame_ms = frame_ms;
    state->rx_audio_applied_preroll_ms = playout_delay_ms;
    state->rx_audio_applied_profile_id = audio_profile_id;
    state->rx_audio_applied_codec_id = codec_id;
    state->rx_audio_applied_valid = 1;
}

/*
 * If v4_audio_chunk.codec_id disagrees with what session_info last announced (e.g. session_info
 * dropped), align decoder state with the wire before inserting the frame. See
 * docs/specs/v4-codec-switching-contract.md.
 */
static void dashcdg_rx_reconcile_v4_audio_codec_from_chunk_locked(
        struct receiver_state *state,
        uint8_t wire_codec_id,
        uint8_t wire_profile_id,
        uint8_t wire_frame_ms
) {
    if (state == NULL || !state->network_audio_enabled || g_audio_decode_disabled ||
            state->announced_audio_sample_rate == 0U) {
        return;
    }
    if (wire_codec_id == state->announced_audio_codec_id && wire_profile_id == state->announced_audio_profile_id &&
            wire_frame_ms == state->announced_audio_frame_ms) {
        return;
    }

    RX_OUT(
            "[rx] v4 audio reconcile: codec %u→%u profile %u→%u frame_ms %u→%u (full audio reconfigure)\n",
            (unsigned int) state->announced_audio_codec_id,
            (unsigned int) wire_codec_id,
            (unsigned int) state->announced_audio_profile_id,
            (unsigned int) wire_profile_id,
            (unsigned int) state->announced_audio_frame_ms,
            (unsigned int) wire_frame_ms
    );

    if (g_audio == NULL) {
        g_audio = dashcdg_desktop_audio_new();
    }
    if (g_audio == NULL) {
        return;
    }

    /*
     * Match session_info codec-change path: stop/init ring + reset g_audio_stream_started so
     * claim_audio_start runs again. Lightweight refresh_* left PortAudio running and wedged audio
     * after multiple codec hops (silent until track change / unrelated reconfigure).
     */
    dashcdg_rx_configure_audio_locked(
            state,
            dashcdg_clock_now_ms(),
            state->announced_audio_sample_rate,
            state->announced_audio_channels,
            wire_frame_ms,
            state->announced_playout_delay_ms,
            wire_profile_id,
            wire_codec_id
    );

    state->announced_audio_frame_ms = wire_frame_ms;
    state->announced_audio_profile_id = wire_profile_id;
    state->announced_audio_codec_id = wire_codec_id;
}

/*
 * After unpause: keep CDG live canvas/jitter intact while refreshing bridge/skip-hold so resume
 * does not fall back to snapshots or blank startup until a later key canvas arrives.
 */
static void dashcdg_rx_rearm_live_video_after_unpause_locked(struct receiver_state *state, uint64_t now_ms) {
    if (state == NULL) {
        return;
    }

    if (state->last_cdg_jitter_apply_local_ms == 0U) {
        state->last_cdg_jitter_apply_local_ms = now_ms;
    }
    if (state->cdg_skip_hold_until_local_ms < now_ms) {
        state->cdg_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
                now_ms,
                dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 0)
        );
    }
    state->v4_loading_screen_active = 0;
    state->v4_bridge_cdg = state->live_state;
    state->v4_bridge_cdg_valid = 1;
}

static void handle_v4_session_info(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int session_changed;
    int song_id_track_changed;
    int asset_metadata_track_change;
    int material_track_change;
    int asset_changed;
    int has_network_audio;
    int need_audio_device_reconfigure;
    int new_v4_session_epoch;
    uint64_t prev_session_start_ms;
    size_t prior_announced_asset_size;

    if (state == NULL || view == NULL) {
        return;
    }

    prev_session_start_ms = state->session_start_ms;
    prior_announced_asset_size = state->asset_size;
    has_network_audio = view->v4_session_info.audio_sample_rate > 0 &&
            view->v4_session_info.audio_channels > 0 &&
            view->v4_session_info.audio_frame_ms > 0;
    session_changed = state->session_start_ms != 0 && state->session_start_ms != view->v4_session_info.session_start_ms;
    /*
     * TX sets session_start_ms from wall ms at track load. Two loads in the same millisecond reuse
     * the same session_start_ms; RX then skipped receiver_state_reset + audio reconfigure, leaving
     * stale jitter/PCM timestamps — claim_audio_start wedged on wait-preroll (common on slower PIII
     * / WinMM paths where "next" hits faster than "back" in practice).
     */
    song_id_track_changed =
            state->song_id[0] != '\0' &&
            strncmp(state->song_id, view->v4_session_info.song_id, sizeof(state->song_id)) != 0;
    /*
     * v4 asset_size is metadata-only, but when it changes while the wire claims the same
     * session_start_ms + song_id (TX same-ms collision or duplicate titles), we still need a full
     * receiver reset so CDG jitter / anchor assembly do not splice two different canvases.
     */
    asset_metadata_track_change =
            prior_announced_asset_size > 0U &&
            (size_t) view->v4_session_info.asset_size != prior_announced_asset_size;
    /*
     * First v4_session_info after a zeroed receiver: session_changed is false because
     * state->session_start_ms is still 0, so without this we skip receiver_state_reset and
     * can leave jitter/clock in a non-session shape until a later edge.
     */
    {
        int cold_session_adopt =
                state->session_start_ms == 0U && view->v4_session_info.session_start_ms != 0U;
        material_track_change = session_changed || song_id_track_changed || asset_metadata_track_change ||
                cold_session_adopt;
    }
    asset_changed = state->asset_size != (size_t) view->v4_session_info.asset_size || state->asset_bytes != NULL;

    if (material_track_change) {
        /*
         * Same as receiver_state_reset: v4_session_info can follow a path that preserves prefetched jitter
         * without calling receiver_state_reset — still treat as a fresh audio epoch for applied-wire state.
         */
        state->rx_audio_applied_valid = 0;
        state->rx_audio_applied_wire_sr = 0U;
        state->rx_audio_applied_wire_ch = 0U;
        state->rx_audio_applied_frame_ms = 0U;
        state->rx_audio_applied_preroll_ms = 0U;
        state->rx_audio_applied_profile_id = 0U;
        state->rx_audio_applied_codec_id = 0U;
        /*
         * Multicast reorder / scheduling can deliver v4_audio_chunk before the first v4_session_info.
         * Cold adopt usually runs receiver_state_reset — which wipes jitter + playback anchors — after we
         * already queued encoded frames and bootstrapped playback_base_* from audio in store_v4_audio_*.
         * That wedge matches “silent until magic key”: toggling decode-drop forces configure_audio.
         */
        int preserve_prefetched_wire_audio =
                state->session_start_ms == 0U &&
                view->v4_session_info.session_start_ms != 0U &&
                dashcdg_audio_jitter_occupied_count(&state->audio_jitter) > 0U &&
                state->playback_base_sender_ms != 0U;

        if (g_audio != NULL) {
            dashcdg_desktop_audio_stop_stream(g_audio);
            dashcdg_desktop_audio_flush_stream_ring(g_audio);
            state->last_observed_stream_underrun_events = g_audio->stream_underrun_events;
            state->last_observed_stream_underrun_frames = g_audio->stream_underrun_frames;
        }
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
        state->audio_last_chunk_local_ms = 0U;
        state->audio_burst_window_start_local_ms = 0U;
        state->audio_burst_run_count = 0U;
        if (!preserve_prefetched_wire_audio) {
            receiver_state_reset(state);
        } else {
            memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
        }
        state->sync_leader_instance_id_low16 = 0U;
        state->sync_leader_trim_bias_ppm = 0;
        state->sync_leader_last_update_local_ms = 0U;
        state->last_session_change_local_ms = local_now_ms;
    }

    /*
     * v4 does not ship full-file assembly on the wire; session_info.asset_size is metadata only.
     * Drop any legacy assembly buffers so RX does not calloc the entire .cdg on join.
     */
    if (state->asset_bytes != NULL) {
        receiver_state_drop_asset_assembly_buffers(state);
    }
    state->asset_size = (size_t) view->v4_session_info.asset_size;

    /*
     * Re-open decoders + PCM ring only when session_info advertises different audio parameters,
     * or the device was never opened. Do NOT key off asset_changed alone: chunk_size stays 0
     * until prepare_asset succeeds, so asset_changed stays true on every periodic session_info
     * (~1 s) — each pass stopped PortAudio, cleared the ring, reset g_audio_stream_started, and
     * HUD fell back to wait-preroll mid-playback.
     */
    /*
     * Compare the *incoming* session_info to the last successful audio setup only.
     * Do not use state->announced_* here: receiver_state_reset() zeroes those while asset_backfill
     * is still settling, which falsely forced reconfigure on every periodic session_info (~1 Hz).
     */
    /*
     * New session_start_ms (track skip) must reopen the output ring and clear g_audio_stream_started
     * even when sample rate / codec / preroll are unchanged; otherwise claim_audio_start stays blocked
     * while the PCM ring drains to zero (silent RX until pause/unpause forces reconfigure).
     */
    need_audio_device_reconfigure =
            material_track_change ||
            (prev_session_start_ms != 0U &&
                    view->v4_session_info.session_start_ms != prev_session_start_ms) ||
            g_audio == NULL ||
            !state->rx_audio_applied_valid ||
            view->v4_session_info.audio_sample_rate != state->rx_audio_applied_wire_sr ||
            view->v4_session_info.audio_channels != state->rx_audio_applied_wire_ch ||
            view->v4_session_info.startup_preroll_ms != state->rx_audio_applied_preroll_ms ||
            view->v4_session_info.audio_codec_id != state->rx_audio_applied_codec_id ||
            view->v4_session_info.audio_profile_id != state->rx_audio_applied_profile_id ||
            view->v4_session_info.audio_frame_ms != state->rx_audio_applied_frame_ms;

    strncpy(state->song_id, view->v4_session_info.song_id, sizeof(state->song_id) - 1U);
    state->song_id[sizeof(state->song_id) - 1U] = '\0';
    new_v4_session_epoch =
            material_track_change || view->v4_session_info.session_start_ms != prev_session_start_ms;
    state->session_start_ms = view->v4_session_info.session_start_ms;
    if (new_v4_session_epoch) {
        state->v4_session_epoch_anchor_sender_ms = view->header.sender_time_ms;
    }
    state->announced_transport_version = DASHCDG_PROTOCOL_VERSION_V4;
    state->announced_audio_sample_rate = view->v4_session_info.audio_sample_rate;
    state->announced_audio_channels = view->v4_session_info.audio_channels;
    state->announced_playout_delay_ms = view->v4_session_info.startup_preroll_ms;
    state->announced_audio_frame_ms = view->v4_session_info.audio_frame_ms;
    state->announced_audio_profile_id = view->v4_session_info.audio_profile_id;
    state->announced_audio_codec_id = view->v4_session_info.audio_codec_id;
    if (view->v4_session_info.repair_mode == DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY) {
        state->announced_audio_fec_group_size = 5U;
        state->announced_cdg_fec_group_size = 9U;
    } else {
        state->announced_audio_fec_group_size = 0U;
        state->announced_cdg_fec_group_size = 0U;
    }
    state->network_audio_enabled = has_network_audio;

    if (material_track_change || asset_changed || !state->have_clock) {
        dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
        state->have_clock = 1;
        dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    }

    if (has_network_audio && !g_audio_decode_disabled && need_audio_device_reconfigure) {
        dashcdg_rx_configure_audio_locked(
                state,
                local_now_ms,
                view->v4_session_info.audio_sample_rate,
                view->v4_session_info.audio_channels,
                view->v4_session_info.audio_frame_ms,
                view->v4_session_info.startup_preroll_ms,
                view->v4_session_info.audio_profile_id,
                view->v4_session_info.audio_codec_id
        );
    } else if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
        state->rx_audio_applied_valid = 0;
        state->playback_base_ms = 0U;
        state->playback_base_sender_ms = 0U;
    }
}

static int dashcdg_rx_store_v4_audio_frame_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->v4_audio_chunk.encoded_bytes == NULL) {
        return 0;
    }

    /*
     * v4_clock_sync normally establishes playback_base_*; if audio arrives first, missing-frame
     * drain needs have_sender_playback — bootstrap from the first chunk's tags until clock_sync
     * overwrites with an authoritative pair.
     * If the first datagrams are audio (reorder) and we have not yet processed session_info / anchor,
     * still bootstrap anchors so dashcdg_rx_sender_playback_now_locked + claim_audio_start work,
     * and anchor sender_clock from this chunk's sender_time_ms so have_clock becomes true.
     */
    if (state->playback_base_sender_ms == 0U) {
        uint64_t now_wall = dashcdg_clock_now_ms();

        state->playback_base_ms = view->v4_audio_chunk.playback_ms;
        state->playback_base_sender_ms = view->header.sender_time_ms;
        if (!state->have_clock) {
            dashcdg_media_clock_anchor(
                    &state->sender_clock,
                    (int64_t) now_wall,
                    (int64_t) view->header.sender_time_ms
            );
            state->have_clock = 1;
            dashcdg_rx_note_clock_update_locked(state, now_wall, 0);
        }
    }

    dashcdg_rx_reconcile_v4_audio_codec_from_chunk_locked(
            state,
            view->v4_audio_chunk.codec_id,
            view->v4_audio_chunk.audio_profile_id,
            view->v4_audio_chunk.frame_ms
    );

    return dashcdg_rx_insert_audio_pending_locked(
            state,
            view->v4_audio_chunk.media_sequence,
            view->v4_audio_chunk.playback_ms,
            view->v4_audio_chunk.frame_ms,
            view->v4_audio_chunk.audio_profile_id,
            view->v4_audio_chunk.codec_id,
            view->v4_audio_chunk.encoded_bytes,
            view->v4_audio_chunk.encoded_length,
            1
    );
}

/*
 * v4_session_info is the normal place we learn sample rate / FEC / preroll. On LAN/multicast the first
 * datagrams can still be v4_audio_chunk (receive-path reorder). Those chunks were dropped entirely
 * because network_audio_enabled stayed 0 — later fixes to store_v4_* never ran.
 */
static int dashcdg_rx_bootstrap_network_audio_from_v4_chunk_locked(
        struct receiver_state *state,
        const struct dashcdg_packet_view *view,
        uint64_t local_now_ms
) {
    uint8_t codec_id;
    uint8_t frame_ms;

    if (state == NULL || view == NULL || state->network_audio_enabled) {
        return 1;
    }
    codec_id = view->v4_audio_chunk.codec_id;
    frame_ms = view->v4_audio_chunk.frame_ms;
    if (frame_ms == 0U ||
            codec_id == 0U ||
            view->v4_audio_chunk.encoded_bytes == NULL ||
            view->v4_audio_chunk.encoded_length == 0U) {
        return 0;
    }

    state->announced_transport_version = DASHCDG_PROTOCOL_VERSION_V4;
    state->announced_audio_sample_rate = DASHCDG_AUDIO_SAMPLE_RATE;
    state->announced_audio_channels = DASHCDG_AUDIO_CHANNELS;
    state->announced_audio_frame_ms = frame_ms;
    /*
     * Chunk does not carry sample rate/channels (desktop TX uses fixed wire layout); narrow codecs use
     * the same expansion-to-48k path — preroll defaults match TX header defaults until session_info refines.
     */
    state->announced_playout_delay_ms = DASHCDG_RX_V4_BOOTSTRAP_PLAYOUT_DELAY_MS;
    state->announced_audio_profile_id = view->v4_audio_chunk.audio_profile_id;
    state->announced_audio_codec_id = codec_id;
    state->announced_audio_fec_group_size = 5U;
    state->announced_cdg_fec_group_size = 9U;
    state->network_audio_enabled = 1;

    if (!state->have_clock) {
        dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
        state->have_clock = 1;
        dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    }

    if (!g_audio_decode_disabled) {
        dashcdg_rx_configure_audio_locked(
                state,
                local_now_ms,
                state->announced_audio_sample_rate,
                (uint8_t) state->announced_audio_channels,
                state->announced_audio_frame_ms,
                state->announced_playout_delay_ms,
                state->announced_audio_profile_id,
                state->announced_audio_codec_id
        );
    }

    RX_OUT("[rx] v4 audio: bootstrapped session params from chunk (session_info not seen yet)\n");
    return 1;
}

static int dashcdg_rx_should_force_audio_epoch_reconfigure_locked(
        const struct receiver_state *state,
        const struct dashcdg_packet_view *view
) {
    uint32_t next_seq;
    uint32_t incoming_seq;

    if (state == NULL || view == NULL || !state->network_audio_enabled || g_audio_decode_disabled) {
        return 0;
    }
    if (!state->audio_jitter.initialized) {
        return 0;
    }

    next_seq = state->audio_jitter.next_media_sequence;
    incoming_seq = view->v4_audio_chunk.media_sequence;
    if (next_seq < 4096U) {
        return 0;
    }
    if (incoming_seq > 512U) {
        return 0;
    }
    if (incoming_seq >= next_seq) {
        return 0;
    }
    if (next_seq - incoming_seq < 2048U) {
        return 0;
    }
    return 1;
}

static void handle_v4_audio_chunk(
        struct receiver_state *state,
        const struct dashcdg_packet_view *view,
        uint64_t local_now_ms
) {
    if (state == NULL || view == NULL || g_audio_decode_disabled) {
        return;
    }
    if (!state->network_audio_enabled &&
            !dashcdg_rx_bootstrap_network_audio_from_v4_chunk_locked(state, view, local_now_ms)) {
        return;
    }
    if (dashcdg_rx_should_force_audio_epoch_reconfigure_locked(state, view) &&
            state->announced_audio_sample_rate > 0U &&
            state->announced_audio_channels > 0U &&
            state->announced_audio_frame_ms > 0U) {
        RX_OUT(
                "[rx] v4 audio epoch-reset: seq regression next=%u incoming=%u (forcing reconfigure)\n",
                (unsigned int) state->audio_jitter.next_media_sequence,
                (unsigned int) view->v4_audio_chunk.media_sequence
        );
        dashcdg_rx_configure_audio_locked(
                state,
                dashcdg_clock_now_ms(),
                state->announced_audio_sample_rate,
                state->announced_audio_channels,
                state->announced_audio_frame_ms,
                state->announced_playout_delay_ms,
                state->announced_audio_profile_id,
                state->announced_audio_codec_id
        );
    }
    if (dashcdg_rx_is_stale_prior_session_media_locked(state, view)) {
        return;
    }

    dashcdg_rx_note_audio_chunk_arrival_locked(state, dashcdg_clock_now_ms());
    if (dashcdg_rx_store_v4_audio_frame_locked(state, view)) {
        dashcdg_rx_observe_audio_payload_for_fec_locked(
                state,
                view->v4_audio_chunk.group_id,
                view->v4_audio_chunk.group_index,
                view->v4_audio_chunk.encoded_bytes,
                view->v4_audio_chunk.encoded_length
        );
    }
}

static void handle_v4_video_delta(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->v4_video_delta.delta_bytes == NULL) {
        return;
    }
    if (dashcdg_rx_is_stale_prior_session_media_locked(state, view)) {
        return;
    }
    if (view->v4_video_delta.delta_format != DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS) {
        return;
    }

    if (dashcdg_rx_insert_cdg_pending_locked(
            state,
            view->v4_video_delta.packet_start_index,
            view->v4_video_delta.packet_count,
            view->v4_video_delta.delta_bytes,
            1
    )) {
        dashcdg_rx_observe_cdg_payload_for_fec_locked(
                state,
                view->v4_video_delta.group_id,
                view->v4_video_delta.group_index,
                view->v4_video_delta.packet_count,
                view->v4_video_delta.delta_bytes
        );
    }
}

static void handle_v4_repair_window(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group = NULL;

    if (state == NULL || view == NULL || view->v4_repair_window.payload_bytes == NULL) {
        return;
    }
    if (dashcdg_rx_is_stale_prior_session_media_locked(state, view)) {
        return;
    }
    if (!(view->v4_repair_window.repair_mode == DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY ||
            view->v4_repair_window.repair_mode == DASHCDG_V4_REPAIR_MODE_VIDEO_WINDOW_XOR)) {
        return;
    }

    if (view->v4_repair_window.group_size <= 1U ||
            view->v4_repair_window.group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->v4_repair_window.payload_length == 0U ||
            view->v4_repair_window.payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return;
    }

    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO && g_audio_decode_disabled) {
        return;
    }

    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, view->v4_repair_window.group_id);
    } else if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, view->v4_repair_window.group_id);
    }
    if (group == NULL) {
        return;
    }

    if (group->expected_group_size == 0 || group->expected_group_size > view->v4_repair_window.group_size) {
        group->expected_group_size = view->v4_repair_window.group_size;
    }
    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG &&
            view->v4_repair_window.redundancy_index < DASHCDG_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
        uint8_t ridx = view->v4_repair_window.redundancy_index;
        if (group->parity_symbol_bytes != (uint8_t) view->v4_repair_window.payload_length) {
            group->parity_symbol_bytes = (uint8_t) view->v4_repair_window.payload_length;
            group->parity_present_mask = 0U;
        }
        memset(group->parity_symbols[ridx], 0, DASHCDG_MAX_FEC_PAYLOAD_BYTES);
        memcpy(
                group->parity_symbols[ridx],
                view->v4_repair_window.payload_bytes,
                view->v4_repair_window.payload_length
        );
        group->parity_present_mask |= (uint8_t) (1U << ridx);
    } else {
        group->parity_present = 1;
        group->parity.payload_bytes = view->v4_repair_window.payload_length;
        group->parity.payload_length_xor = view->v4_repair_window.payload_length;
        memcpy(group->parity.payload_xor, view->v4_repair_window.payload_bytes, view->v4_repair_window.payload_length);
    }

    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        dashcdg_rx_try_recover_audio_group_locked(state, group);
    } else if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        dashcdg_rx_try_recover_cdg_group_locked(state, group);
    }
}

static void handle_v4_clock_sync(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int was_paused;
    int session_epoch_changed;
    uint32_t startup_state;
    uint16_t leader_id_low16;
    int16_t leader_trim_ppm;

    if (state == NULL || view == NULL) {
        return;
    }

    was_paused = state->playback_paused;
    session_epoch_changed =
            state->session_start_ms != 0U &&
            state->session_start_ms != view->v4_clock_sync.session_start_ms;
    dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
    state->have_clock = 1;
    state->session_start_ms = view->v4_clock_sync.session_start_ms;
    if (session_epoch_changed) {
        state->v4_session_epoch_anchor_sender_ms = view->header.sender_time_ms;
    }
    state->playback_base_ms = view->v4_clock_sync.playback_ms;
    state->playback_base_sender_ms = view->header.sender_time_ms;
    state->playback_paused = (view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0;
    startup_state = view->v4_clock_sync.startup_state;
    if ((startup_state & DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MAGIC_MASK) == DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MAGIC) {
        state->sync_group_mode = (uint8_t) ((startup_state >> DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MODE_SHIFT) &
                DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_MODE_MASK);
        state->sync_group_target_latency_ms = (uint16_t) ((startup_state >> DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_TARGET_SHIFT) &
                DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_TARGET_MASK);
        state->sync_group_phase_spread_ms = (uint8_t) ((startup_state >> DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_SHIFT) &
                DASHCDG_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_MASK);
    } else {
        state->sync_group_mode = DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE;
        state->sync_group_target_latency_ms = 0U;
        state->sync_group_phase_spread_ms = 0U;
    }
    leader_id_low16 = (uint16_t) ((view->v4_clock_sync.reserved >> 16U) & 0xffffU);
    leader_trim_ppm = (int16_t) (view->v4_clock_sync.reserved & 0xffffU);
    state->sync_leader_instance_id_low16 = leader_id_low16;
    state->sync_leader_trim_bias_ppm = leader_trim_ppm;
    state->sync_leader_last_update_local_ms = local_now_ms;
    dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    if (session_epoch_changed &&
            state->network_audio_enabled &&
            !g_audio_decode_disabled &&
            state->announced_audio_sample_rate > 0U &&
            state->announced_audio_channels > 0U &&
            state->announced_audio_frame_ms > 0U) {
        RX_OUT(
                "[rx] v4 clock epoch: session_start changed to %llu (forcing audio reconfigure)\n",
                (unsigned long long) state->session_start_ms
        );
        dashcdg_rx_configure_audio_locked(
                state,
                local_now_ms,
                state->announced_audio_sample_rate,
                state->announced_audio_channels,
                state->announced_audio_frame_ms,
                state->announced_playout_delay_ms,
                state->announced_audio_profile_id,
                state->announced_audio_codec_id
        );
        state->last_session_change_local_ms = local_now_ms;
    }
    if (was_paused && !state->playback_paused) {
        dashcdg_rx_rearm_live_video_after_unpause_locked(state, local_now_ms);
        dashcdg_rx_reprime_audio_after_host_underrun_locked(state);
    }
}

static void handle_asset_chunk(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;
    size_t old_prefix = 0;

    if (state->asset_bytes == NULL || state->chunk_seen == NULL) {
        return;
    }

    if ((size_t) view->asset_chunk.asset_offset + view->asset_chunk.chunk_length > state->asset_size) {
        return;
    }

    memcpy(
            state->asset_bytes + view->asset_chunk.asset_offset,
            view->asset_chunk.chunk_bytes,
            view->asset_chunk.chunk_length
    );
    state->asset_bytes_written += view->asset_chunk.chunk_length;

    chunk_index = view->asset_chunk.asset_offset / state->chunk_size;
    if (chunk_index < state->chunk_count && state->chunk_seen[chunk_index] == 0) {
        state->chunk_seen[chunk_index] = 1;
        state->received_chunks++;
    } else if (chunk_index < state->chunk_count) {
        state->duplicate_chunks++;
    }

    old_prefix = state->contiguous_prefix_chunks;
    receiver_state_refresh_prefix(state);
    if (state->contiguous_prefix_chunks != old_prefix || state->received_chunks == state->chunk_count) {
        state->last_progress_local_ms = dashcdg_clock_now_ms();
    }

    receiver_state_try_finalize(state);
}

static void handle_clock_beacon(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int was_paused;

    was_paused = state->playback_paused;
    dashcdg_media_clock_observe(
            &state->sender_clock,
            (int64_t) local_now_ms,
            (int64_t) view->header.sender_time_ms,
            20
    );
    dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    state->session_start_ms = view->clock_beacon.session_start_ms;
    state->playback_base_ms = view->clock_beacon.playback_ms;
    state->playback_base_sender_ms = view->header.sender_time_ms;
    state->playback_paused = (view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0;
    if (was_paused && !state->playback_paused) {
        dashcdg_rx_rearm_live_video_after_unpause_locked(state, local_now_ms);
        dashcdg_rx_reprime_audio_after_host_underrun_locked(state);
    }
}

static void dashcdg_rx_init_stats_sender(int stats_port) {
    int ttl = 1;
    unsigned char loopback = 1;
    int reuse = 1;
    struct sockaddr_in local_addr;
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;

    if (g_rx_stats_interval_ms == 0U) {
        return;
    }
    g_rx_stats_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_stats_sockfd == DASHCDG_INVALID_SOCKET) {
        perror("[rx] stats socket");
        return;
    }
    if (!dashcdg_net_set_dscp(g_rx_stats_sockfd, DASHCDG_NET_DSCP_DASHCDG_DEFAULT)) {
        perror("[rx] stats IP_TOS");
    }
    if (g_endpoint_is_multicast) {
        multicast_interface_count = dashcdg_net_list_multicast_interfaces(
                multicast_interfaces,
                DASHCDG_MAX_MULTICAST_INTERFACES
        );
        if (multicast_interface_count > 0U &&
                !dashcdg_net_set_multicast_interface(g_rx_stats_sockfd, &multicast_interfaces[0].ipv4_addr)) {
            perror("[rx] stats IP_MULTICAST_IF");
        }
        if (setsockopt(g_rx_stats_sockfd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0) {
            perror("[rx] stats IP_MULTICAST_TTL");
        }
        if (setsockopt(g_rx_stats_sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loopback, sizeof(loopback)) != 0) {
            perror("[rx] stats IP_MULTICAST_LOOP");
        }
    } else if (g_endpoint_is_broadcast) {
        int enable_broadcast = 1;

        if (setsockopt(
                    g_rx_stats_sockfd,
                    SOL_SOCKET,
                    SO_BROADCAST,
                    (const char *) &enable_broadcast,
                    sizeof(enable_broadcast)
            ) != 0) {
            perror("[rx] stats SO_BROADCAST");
        }
    }

    memset(&g_rx_stats_dest, 0, sizeof(g_rx_stats_dest));
    g_rx_stats_dest.sin_family = AF_INET;
    g_rx_stats_dest.sin_port = htons((uint16_t) stats_port);
    g_rx_stats_dest.sin_addr = g_endpoint_in_addr;
    g_rx_repair_nack_dest = g_rx_stats_dest;
    if (g_endpoint_is_multicast) {
        struct in_addr nack_grp;
        if (dashcdg_rx_parse_ipv4_address(DASHCDG_V4_REPAIR_NACK_MCAST_ADDR_STR, &nack_grp)) {
            g_rx_repair_nack_dest.sin_addr = nack_grp;
        }
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((uint16_t) stats_port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(g_rx_stats_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
    if (bind(g_rx_stats_sockfd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0) {
        perror("[rx] stats bind");
    } else if (g_endpoint_is_multicast) {
        (void)dashcdg_rx_join_multicast_interfaces(
                g_rx_stats_sockfd,
                &g_endpoint_in_addr,
                multicast_interfaces,
                multicast_interface_count
        );
    }
}

/** Avoid full parse for repair-nack / other control on stats port (badge may multicast before TX IP is known). */
static int dashcdg_rx_stats_datagram_is_v4_rx_stats(const uint8_t *p, size_t len)
{
    uint32_t magic;
    if (len < 6U) {
        return 0;
    }
    magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (magic != DASHCDG_PROTOCOL_MAGIC) {
        return 0;
    }
    if (p[4] != DASHCDG_PROTOCOL_VERSION && p[4] != DASHCDG_PROTOCOL_VERSION_V4) {
        return 0;
    }
    return p[5] == (uint8_t)DASHCDG_PACKET_V4_RX_STATS;
}

static const char *dashcdg_rx_group_sync_mode_label(uint8_t mode) {
    switch (mode) {
        case DASHCDG_TX_GROUP_SYNC_MODE_OFF:
            return "off";
        case DASHCDG_TX_GROUP_SYNC_MODE_MEASURE:
            return "measure";
        case DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE:
            return "active";
        default:
            return "unknown";
    }
}

static void *dashcdg_rx_stats_thread_main(void *unused) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

    (void)unused;
    for (;;) {
        struct sockaddr_in src;
        socklen_t src_len = (socklen_t)sizeof(src);
        int received;
        struct dashcdg_packet_view view;

        received = (int)recvfrom(
                g_rx_stats_sockfd,
                (char *)packet,
                sizeof(packet),
                0,
                (struct sockaddr *)&src,
                &src_len
        );
        if (received <= 0) {
            break;
        }
        if (!dashcdg_rx_stats_datagram_is_v4_rx_stats(packet, (size_t)received)) {
            continue;
        }
        if (!dashcdg_protocol_parse_packet(&view, packet, (size_t)received)) {
            continue;
        }
        if (view.header.type != DASHCDG_PACKET_V4_RX_STATS) {
            continue;
        }
        pthread_mutex_lock(&g_receiver.mutex);
        if (view.v4_rx_stats.receiver_instance_id != g_rx_receiver_instance_id) {
            g_receiver.v4_rx_stats_peer_packets++;
        }
        pthread_mutex_unlock(&g_receiver.mutex);
    }
    return NULL;
}

static void dashcdg_rx_request_shutdown(void) {
    g_rx_shutdown_requested = 1;
    pthread_mutex_lock(&g_rx_nack_queue_mutex);
    pthread_cond_broadcast(&g_rx_nack_queue_cond);
    pthread_mutex_unlock(&g_rx_nack_queue_mutex);
    if (g_rx_data_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_rx_data_sockfd);
        g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
    }
    if (g_rx_stats_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_rx_stats_sockfd);
        g_rx_stats_sockfd = DASHCDG_INVALID_SOCKET;
    }
}

static void dashcdg_rx_maybe_send_v4_stats_locked(uint64_t now_ms) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_rx_stats_payload pl;
    size_t sz;
    uint64_t sender_observed_ms;
    uint32_t audio_buffered_ms = 0U;
    uint32_t audio_target_buffer_ms;
    uint32_t audio_host_latency_ms;
    uint32_t audio_target_total_ms;
    int audio_ts = -1;
    int summary_due = 0;

    if (g_rx_stats_interval_ms == 0U || g_rx_stats_sockfd == DASHCDG_INVALID_SOCKET) {
        return;
    }
    if (g_receiver.announced_transport_version != DASHCDG_PROTOCOL_VERSION_V4 || !g_receiver.network_audio_enabled) {
        return;
    }
    if (g_receiver.rx_stats_last_sent_local_ms != 0U &&
            now_ms - g_receiver.rx_stats_last_sent_local_ms < (uint64_t) g_rx_stats_interval_ms) {
        return;
    }

    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));
    hdr.sequence = ++g_receiver.rx_stats_report_seq;
    hdr.sender_time_ms = now_ms;

    if (g_receiver.have_clock) {
        sender_observed_ms = (uint64_t) dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) now_ms);
    } else {
        sender_observed_ms = 0U;
    }

    audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    audio_target_buffer_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    audio_host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    audio_target_total_ms = dashcdg_rx_audio_target_total_latency_ms_locked(&g_receiver);
    if (g_audio != NULL) {
        audio_ts = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    }
    audio_buffered_ms = dashcdg_rx_stats_sanitized_audio_buffer_ms_locked(&g_receiver, audio_buffered_ms);
    if (audio_ts < 0) {
        audio_ts = 0;
    }
    if (audio_ts == 0 && audio_buffered_ms == 0U) {
        audio_host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    }

    pl.report_seq = g_receiver.rx_stats_report_seq;
    pl.wall_now_ms = now_ms;
    pl.sender_time_observed_ms = sender_observed_ms;
    pl.clock_offset_estimate_ms = (int32_t) g_receiver.sender_offset_ms;
    pl.playout_delay_ms_config = (uint16_t) (audio_target_total_ms > 65535U ? 65535U : audio_target_total_ms);
    pl.audio_buffer_ms = audio_buffered_ms;
    pl.audio_queue_pressure_events = (uint32_t) g_receiver.audio_queue_overflows;
    pl.fec_audio_recovered = (uint32_t) g_receiver.fec_audio_recovered;
    pl.jitter_rms_ms = (uint16_t) (g_receiver.rx_interarrival_jitter_ema_ms > 65535U ? 65535U : g_receiver.rx_interarrival_jitter_ema_ms);
    {
        uint64_t loss_est = g_receiver.live_missing_skips + g_receiver.audio_missing_skips;
        uint64_t loss_x100 = 0U;
        if (g_receiver.datagrams_received > 0U) {
            loss_x100 = (loss_est * 10000ULL) / g_receiver.datagrams_received;
        }
        if (loss_x100 > 65535ULL) {
            loss_x100 = 65535ULL;
        }
        pl.loss_pct_x100 = (uint16_t) loss_x100;
        pl.media_datagrams_lost_estimated = (uint32_t) (loss_est > 0xffffffffULL ? 0xffffffffULL : loss_est);
    }
    pl.v4_codec_id = g_receiver.announced_audio_codec_id;
    pl.opus_bitrate_bps = 0U;
    pl.fec_decode_attempts = (uint32_t) (g_receiver.fec_audio_recovered + g_receiver.fec_cdg_recovered +
            g_receiver.fec_recovery_failures);
    pl.fec_recovery_failed = (uint32_t) g_receiver.fec_recovery_failures;
    pl.cdg_fec_recovered = (uint32_t) g_receiver.fec_cdg_recovered;
    pl.cdg_fec_failed = (uint32_t) g_receiver.fec_recovery_failures;
    pl.jitter_p95_ms = pl.jitter_rms_ms;
    pl.jitter_max_ms = pl.jitter_rms_ms;
    pl.reorder_events = (uint32_t) (g_receiver.audio_jitter.reordered_packets + g_receiver.cdg_batch_jitter.reordered_batches);
    pl.receiver_instance_id = g_rx_receiver_instance_id;
    pl.fec_group_size_observed = g_receiver.announced_audio_fec_group_size;
    pl.presented_audio_timestamp_ms = audio_ts > 0 ? (uint32_t) audio_ts : 0U;
    pl.audio_buffer_target_ms = (uint16_t) (audio_target_buffer_ms > 65535U ? 65535U : audio_target_buffer_ms);
    pl.host_output_latency_ms = (uint16_t) (audio_host_latency_ms > 65535U ? 65535U : audio_host_latency_ms);
    pl.target_total_latency_ms = (uint16_t) (audio_target_total_ms > 65535U ? 65535U : audio_target_total_ms);
    pl.startup_stage = dashcdg_rx_stats_sanitized_startup_stage_locked(&g_receiver, now_ms, audio_buffered_ms);
    pl.drift_trim_ppm = g_receiver.audio_resample_trim_ppm;
    pl.recovery_host_underrun_count = (uint32_t) g_receiver.recovery_host_underrun_count;
    pl.recovery_zero_buffer_count = (uint32_t) g_receiver.recovery_zero_buffer_count;
    pl.recovery_silent_stall_count = (uint32_t) g_receiver.recovery_silent_stall_count;
    pl.source_idle_park_count = (uint32_t) g_receiver.source_idle_park_count;
    pl.startup_flags = dashcdg_rx_startup_flags_locked(&g_receiver, now_ms, audio_buffered_ms);
    pl.video_jb_pending_slots = (uint32_t)dashcdg_cdg_batch_jitter_occupied_count(&g_receiver.cdg_batch_jitter);
    pl.video_jb_next_packet_index = g_receiver.cdg_batch_jitter.next_packet_index;
    pl.v4_clock_rx_count = (uint32_t)g_receiver.v4_clock_sync_packets;
    pl.clock_skew_ema_ms = (int32_t) g_receiver.rx_sender_skew_ema_ms;
    pl.ptp_offset_ema_us = 0;
    pl.heap_free_min_bytes = 0U;
    pl.wifi_rssi_dbm = 0;
    pl.ptp_mode = 2U;
    pl.stats_generation = 4U;
    pl.device_flags = g_headless ? 0x1U : 0x0U;

    sz = dashcdg_protocol_serialize_v4_rx_stats(packet, sizeof(packet), &hdr, &pl);
    if (sz == 0U) {
        return;
    }
    if (sendto(
                g_rx_stats_sockfd,
                (const char *) packet,
                (int) sz,
                0,
                (struct sockaddr *) &g_rx_stats_dest,
                sizeof(g_rx_stats_dest)
        ) != (int) sz) {
        perror("[rx] send v4 rx-stats");
        return;
    }
    g_receiver.rx_stats_last_sent_local_ms = now_ms;
    if (g_receiver.rx_stats_last_summary_local_ms == 0U ||
            now_ms - g_receiver.rx_stats_last_summary_local_ms >= DASHCDG_RX_STATS_SUMMARY_INTERVAL_MS) {
        summary_due = 1;
        g_receiver.rx_stats_last_summary_local_ms = now_ms;
    }
    if (summary_due) {
        char line[256];
        uint64_t repair_total = g_receiver.fec_cdg_recovered + g_receiver.fec_recovery_failures;
        uint32_t repair_rate_x100 = repair_total == 0U ? 0U
            : (uint32_t) ((g_receiver.fec_cdg_recovered * 10000ULL) / repair_total);

        snprintf(
                line,
                sizeof(line),
                "[rx] v4-stats\tseq=%u\tsnd=%" DASHCDG_RX_PRIu64 "ms\tpts=%u\tbuf=%ums\ttgt=%u/%u\thost=%u\ttrim=%d\tleader=%u/%d\tgmode=%s\tgtarget=%u\tgspread=%u\tstage=%u\trec=%u/%u/%u\tidle=%u\tnack_tx=%" DASHCDG_RX_PRIu64 "\tunrec=%" DASHCDG_RX_PRIu64 "\teff=%u.%02u%%",
                (unsigned int) pl.report_seq,
                (uint64_t) pl.sender_time_observed_ms,
                (unsigned int) pl.presented_audio_timestamp_ms,
                (unsigned int) pl.audio_buffer_ms,
                (unsigned int) pl.audio_buffer_target_ms,
                (unsigned int) pl.target_total_latency_ms,
                (unsigned int) pl.host_output_latency_ms,
                (int) pl.drift_trim_ppm,
                (unsigned int) g_receiver.sync_leader_instance_id_low16,
                (int) g_receiver.sync_leader_trim_bias_ppm,
                dashcdg_rx_group_sync_mode_label(g_receiver.sync_group_mode),
                (unsigned int) g_receiver.sync_group_target_latency_ms,
                (unsigned int) g_receiver.sync_group_phase_spread_ms,
                (unsigned int) pl.startup_stage,
                (unsigned int) pl.recovery_host_underrun_count,
                (unsigned int) pl.recovery_zero_buffer_count,
                (unsigned int) pl.recovery_silent_stall_count,
                (unsigned int) pl.source_idle_park_count,
                (uint64_t) g_receiver.repair_nack_tx,
                (uint64_t) g_receiver.cdg_unrecoverable_groups,
                (unsigned int) (repair_rate_x100 / 100U),
                (unsigned int) (repair_rate_x100 % 100U)
        );
        dashcdg_rx_async_stdout_line(line);
    }
}

static int dashcdg_rx_nack_queue_pop(struct dashcdg_rx_nack_job *job) {
    if (job == NULL) {
        return 0;
    }
    pthread_mutex_lock(&g_rx_nack_queue_mutex);
    while (g_rx_nack_queue_count == 0U && !g_rx_shutdown_requested) {
        pthread_cond_wait(&g_rx_nack_queue_cond, &g_rx_nack_queue_mutex);
    }
    if (g_rx_nack_queue_count == 0U) {
        pthread_mutex_unlock(&g_rx_nack_queue_mutex);
        return 0;
    }
    *job = g_rx_nack_queue[g_rx_nack_queue_head];
    g_rx_nack_queue_head = (g_rx_nack_queue_head + 1U) % DASHCDG_RX_NACK_QUEUE_CAPACITY;
    g_rx_nack_queue_count--;
    pthread_mutex_unlock(&g_rx_nack_queue_mutex);
    return 1;
}

static void dashcdg_rx_send_v4_repair_nack_now(
        uint64_t now_ms,
        uint8_t stream_type,
        uint32_t group_id,
        uint16_t observed_group_size,
        uint16_t missing_mask
) {
    static uint64_t s_last_nack_ms;
    static uint32_t s_last_nack_group_id;
    static uint16_t s_last_nack_mask;
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_repair_nack_payload pl;
    size_t sz;

    if (g_rx_stats_sockfd == DASHCDG_INVALID_SOCKET || missing_mask == 0U) {
        return;
    }
    if (s_last_nack_group_id == group_id && s_last_nack_mask == missing_mask &&
            s_last_nack_ms != 0U && now_ms > s_last_nack_ms &&
            now_ms - s_last_nack_ms < DASHCDG_RX_REPAIR_NACK_COOLDOWN_MS) {
        return;
    }
    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));
    hdr.sequence = ++g_receiver.rx_stats_report_seq;
    hdr.sender_time_ms = now_ms;
    pl.stream_type = stream_type;
    pl.observed_group_size = observed_group_size;
    pl.group_id = group_id;
    pl.missing_member_mask = missing_mask;
    sz = dashcdg_protocol_serialize_v4_repair_nack(packet, sizeof(packet), &hdr, &pl);
    if (sz == 0U) {
        return;
    }
    if (sendto(
                g_rx_stats_sockfd,
                (const char *) packet,
                (int) sz,
                0,
                (struct sockaddr *) &g_rx_repair_nack_dest,
                sizeof(g_rx_repair_nack_dest)
        ) == (int) sz) {
        s_last_nack_ms = now_ms;
        s_last_nack_group_id = group_id;
        s_last_nack_mask = missing_mask;
        g_receiver.repair_nack_tx++;
        RX_OUT("[rx] repair-nack: group=%u size=%u mask=0x%04x\n", (unsigned int) group_id,
               (unsigned int) observed_group_size, (unsigned int) missing_mask);
    }
}

static void *dashcdg_rx_repair_nack_thread_main(void *unused) {
    struct dashcdg_rx_nack_job job;

    (void) unused;
    while (dashcdg_rx_nack_queue_pop(&job)) {
        dashcdg_rx_send_v4_repair_nack_now(
                job.now_ms,
                job.stream_type,
                job.group_id,
                job.observed_group_size,
                job.missing_mask
        );
    }
    return NULL;
}

static void dashcdg_rx_send_v4_repair_nack_locked(
        uint64_t now_ms,
        uint8_t stream_type,
        uint32_t group_id,
        uint16_t observed_group_size,
        uint16_t missing_mask
) {
    if (missing_mask == 0U || g_receiver.announced_transport_version != DASHCDG_PROTOCOL_VERSION_V4) {
        return;
    }
    pthread_mutex_lock(&g_rx_nack_queue_mutex);
    if (g_rx_nack_queue_count >= DASHCDG_RX_NACK_QUEUE_CAPACITY) {
        g_rx_nack_queue_dropped++;
    } else {
        struct dashcdg_rx_nack_job *job = &g_rx_nack_queue[g_rx_nack_queue_tail];

        job->now_ms = now_ms;
        job->stream_type = stream_type;
        job->group_id = group_id;
        job->observed_group_size = observed_group_size;
        job->missing_mask = missing_mask;
        g_rx_nack_queue_tail = (g_rx_nack_queue_tail + 1U) % DASHCDG_RX_NACK_QUEUE_CAPACITY;
        g_rx_nack_queue_count++;
        pthread_cond_signal(&g_rx_nack_queue_cond);
    }
    pthread_mutex_unlock(&g_rx_nack_queue_mutex);
}

static void *network_thread(void *user_data) {
    struct dashcdg_win32_mmcss_handle mmcss;
    int port = *(int *) user_data;
    dashcdg_socket_t sockfd;
    struct dashcdg_udp_rx_config rx_cfg;
    struct sockaddr_in endpoint_addr;
    struct sockaddr_in sender_addr;
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;
    size_t joined_interface_count = 0U;

    memset(&rx_cfg, 0, sizeof(rx_cfg));
    rx_cfg.port_host_order = (uint16_t) port;
    rx_cfg.is_broadcast_endpoint = g_endpoint_is_broadcast ? 1 : 0;
    if (!dashcdg_transport_udp_socket_init_rx(&rx_cfg, &sockfd)) {
        return NULL;
    }

    if (g_endpoint_is_multicast) {
        multicast_interface_count = dashcdg_net_list_multicast_interfaces(
                multicast_interfaces,
                DASHCDG_MAX_MULTICAST_INTERFACES
        );
        if (multicast_interface_count > 0U &&
                !dashcdg_net_set_multicast_interface(sockfd, &multicast_interfaces[0].ipv4_addr)) {
            perror("IP_MULTICAST_IF");
            dashcdg_socket_close(sockfd);
            return NULL;
        }
        joined_interface_count = dashcdg_rx_join_multicast_interfaces(
                sockfd,
                &g_endpoint_in_addr,
                multicast_interfaces,
                multicast_interface_count
        );
        if (joined_interface_count == 0U) {
            perror("IP_ADD_MEMBERSHIP");
            dashcdg_socket_close(sockfd);
            return NULL;
        }
        if (multicast_interface_count > 0U) {
            char preferred_interface[192];

            dashcdg_rx_format_multicast_interface(&multicast_interfaces[0], preferred_interface, sizeof(preferred_interface));
            RX_OUT(
                    "[rx] multicast preferred interface: %s (joined on %u interface%s)\n",
                    preferred_interface,
                    (unsigned int) joined_interface_count,
                    joined_interface_count == 1U ? "" : "s"
            );
        }
    }

    dashcdg_win32_thread_timing_boost_begin(&mmcss);

    memset(&endpoint_addr, 0, sizeof(endpoint_addr));
    endpoint_addr.sin_family = AF_INET;
    endpoint_addr.sin_port = htons((uint16_t) g_rx_stats_port);
    endpoint_addr.sin_addr = g_endpoint_in_addr;
    g_rx_data_sockfd = sockfd;

    for (;;) {
        size_t received = 0U;

        if (!dashcdg_transport_udp_recv_datagram(sockfd, buffer, sizeof(buffer), &sender_addr, &received) || received == 0U) {
            if (g_rx_shutdown_requested) {
                break;
            }
            continue;
        }

        {
            struct dashcdg_packet_view view;
            uint64_t local_now_ms = dashcdg_clock_now_ms();

            pthread_mutex_lock(&g_receiver.mutex);
            {
                uint64_t prev_dg = g_receiver.last_datagram_local_ms;

                g_receiver.datagrams_received++;
                g_receiver.bytes_received += (uint64_t) received;
                g_receiver.last_datagram_local_ms = local_now_ms;
                if (g_rx_stats_interval_ms != 0U && prev_dg > 0U && local_now_ms > prev_dg) {
                    uint64_t gap = local_now_ms - prev_dg;
                    uint64_t err = gap > 25U ? gap - 25U : 25U - gap;

                    g_receiver.rx_interarrival_jitter_ema_ms =
                            (g_receiver.rx_interarrival_jitter_ema_ms * 7U + (uint32_t) err) / 8U;
                }
            }

            if (!dashcdg_protocol_parse_packet(&view, buffer, received)) {
                g_receiver.parse_failures++;
                pthread_mutex_unlock(&g_receiver.mutex);
                continue;
            }
            {
                int64_t skew = (int64_t) view.header.sender_time_ms - (int64_t) local_now_ms;

                if (g_rx_stats_interval_ms != 0U || g_hud_visible) {
                    if (!g_receiver.rx_sender_skew_ema_inited) {
                        g_receiver.rx_sender_skew_ema_ms = skew;
                        g_receiver.rx_sender_skew_ema_inited = 1;
                    } else {
                        g_receiver.rx_sender_skew_ema_ms = (g_receiver.rx_sender_skew_ema_ms * 7 + skew) / 8;
                    }
                }
            }

            switch (view.header.type) {
                case DASHCDG_PACKET_ANNOUNCE:
                    g_receiver.announce_packets++;
                    handle_announce(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_ASSET_CHUNK:
                    g_receiver.asset_chunk_packets++;
                    handle_asset_chunk(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CLOCK_BEACON:
                    g_receiver.clock_beacon_packets++;
                    handle_clock_beacon(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_AUDIO_FRAME:
                    g_receiver.audio_packets++;
                    handle_audio_frame(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CDG_BATCH:
                    g_receiver.cdg_batch_packets++;
                    handle_live_cdg_batch(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_FEC_PARITY:
                    g_receiver.fec_packets++;
                    if (view.fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
                        g_receiver.fec_audio_packets++;
                    } else if (view.fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
                        g_receiver.fec_cdg_packets++;
                    }
                    /* Legacy XOR parity path intentionally ignored; v4 repair-window path is authoritative. */
                    break;
                case DASHCDG_PACKET_CDG_SNAPSHOT:
                    g_receiver.cdg_snapshot_packets++;
                    dashcdg_rx_handle_snapshot_locked(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_PTP_SYNC:
                    g_receiver.ptp_sync_packets++;
                    g_receiver.pending_sync_id = view.ptp_sync.sync_id;
                    g_receiver.pending_sync_rx_local_ms = local_now_ms;
                    g_receiver.pending_sync_valid = 1;
                    break;
                case DASHCDG_PACKET_PTP_FOLLOW_UP:
                    g_receiver.ptp_follow_up_packets++;
                    if (g_receiver.pending_sync_valid &&
                            view.ptp_follow_up.sync_id == g_receiver.pending_sync_id &&
                            local_now_ms - g_receiver.pending_sync_rx_local_ms <= DASHCDG_MAX_PTP_EXCHANGE_AGE_MS) {
                        g_receiver.pending_sync_origin_remote_ms = view.ptp_follow_up.origin_time_ms;
                        send_ptp_delay_request(&g_receiver, sockfd, &endpoint_addr, local_now_ms);
                    } else {
                        g_receiver.pending_sync_valid = 0;
                        dashcdg_media_clock_observe(&g_receiver.sender_clock, (int64_t) local_now_ms, (int64_t) view.ptp_follow_up.origin_time_ms, 5);
                        dashcdg_rx_note_clock_update_locked(&g_receiver, local_now_ms, 0);
                    }
                    break;
                case DASHCDG_PACKET_PTP_DELAY_REQ:
                    break;
                case DASHCDG_PACKET_PTP_DELAY_RESP:
                    g_receiver.ptp_delay_resp_packets++;
                    if (g_receiver.pending_sync_valid && g_receiver.pending_delay_request_valid &&
                            local_now_ms >= g_receiver.pending_delay_request_local_ms &&
                            local_now_ms - g_receiver.pending_delay_request_local_ms <= DASHCDG_MAX_PTP_EXCHANGE_AGE_MS &&
                            view.ptp_delay_resp.request_id == g_receiver.pending_delay_request_id) {
                        dashcdg_media_clock_observe_ptp_exchange(
                                &g_receiver.sender_clock,
                                (int64_t) g_receiver.pending_sync_origin_remote_ms,
                                (int64_t) g_receiver.pending_sync_rx_local_ms,
                                (int64_t) g_receiver.pending_delay_request_local_ms,
                                (int64_t) view.ptp_delay_resp.request_rx_time_ms,
                                5,
                                5
                        );
                        dashcdg_rx_note_clock_update_locked(&g_receiver, local_now_ms, 1);
                        g_receiver.pending_sync_valid = 0;
                        g_receiver.pending_delay_request_valid = 0;
                    } else if (g_receiver.pending_delay_request_valid &&
                            local_now_ms >= g_receiver.pending_delay_request_local_ms &&
                            local_now_ms - g_receiver.pending_delay_request_local_ms > DASHCDG_MAX_PTP_EXCHANGE_AGE_MS) {
                        g_receiver.pending_sync_valid = 0;
                        g_receiver.pending_delay_request_valid = 0;
                    }
                    break;
                case DASHCDG_PACKET_V4_SESSION_INFO:
                    g_receiver.v4_session_info_packets++;
                    handle_v4_session_info(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_V4_LOADING_SCREEN:
                    g_receiver.v4_loading_screen_packets++;
                    dashcdg_rx_apply_loading_screen_locked(
                            &g_receiver,
                            view.v4_loading_screen.screen_kind,
                            view.v4_loading_screen.animation_phase
                    );
                    break;
                case DASHCDG_PACKET_V4_VIDEO_ANCHOR:
                    g_receiver.v4_video_anchor_packets++;
                    dashcdg_rx_handle_v4_anchor_locked(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_AUDIO_CHUNK:
                    g_receiver.v4_audio_chunk_packets++;
                    g_receiver.v4_loading_screen_active = 0;
                    handle_v4_audio_chunk(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_V4_VIDEO_DELTA:
                    g_receiver.v4_video_delta_packets++;
                    g_receiver.v4_loading_screen_active = 0;
                    handle_v4_video_delta(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_REPAIR_WINDOW:
                    g_receiver.v4_repair_window_packets++;
                    g_receiver.fec_packets++;
                    if (view.v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
                        g_receiver.fec_audio_packets++;
                    } else if (view.v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
                        g_receiver.fec_cdg_packets++;
                    }
                    handle_v4_repair_window(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_CLOCK_SYNC:
                    g_receiver.v4_clock_sync_packets++;
                    handle_v4_clock_sync(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_V4_RX_STATS:
                    g_receiver.v4_rx_stats_peer_packets++;
                    break;
                default:
                    g_receiver.unknown_packets++;
                    break;
            }
            pthread_mutex_unlock(&g_receiver.mutex);
        }
    }

    if (g_rx_data_sockfd != DASHCDG_INVALID_SOCKET) {
        g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
        dashcdg_socket_close(sockfd);
    }
    return NULL;
}

static int dashcdg_rx_claim_audio_start_locked(uint64_t local_now_ms);
static void dashcdg_rx_fill_hud_lines_locked(
        uint64_t local_now_ms,
        char *hud_line_a,
        size_t hud_line_a_size,
        char *hud_line_b,
        size_t hud_line_b_size,
        uint32_t *hud_line_a_rgb,
        uint32_t *hud_line_b_rgb
);

static void dashcdg_rx_start_audio_async(void) {
    struct dashcdg_desktop_audio *audio = NULL;
    int started_ok = 0;
    int stream_active_now = 0;
    uint64_t now_ms = dashcdg_clock_now_ms();

    /*
     * Do not hold g_receiver.mutex across start_stream: Pa_OpenStream/Pa_StartStream can block long
     * enough to starve packet handling and trigger buf=0ms / CDG continuity skips. Lifecycle races are
     * serialized in desktop_audio via g_dashcdg_pa_stream_mutex.
     */
    pthread_mutex_lock(&g_receiver.mutex);
    if (!g_audio_start_inflight && g_audio != NULL) {
        g_audio_start_inflight = 1;
        audio = g_audio;
    }
    if (audio == NULL) {
        pthread_mutex_unlock(&g_receiver.mutex);
        return;
    }
    pthread_mutex_unlock(&g_receiver.mutex);

    started_ok = dashcdg_desktop_audio_start_stream(audio);

    pthread_mutex_lock(&g_receiver.mutex);
#if DASHCDG_HAVE_PORTAUDIO
    if (started_ok && g_audio == audio && audio->stream != NULL && Pa_IsStreamActive(audio->stream) == 1) {
        stream_active_now = 1;
    }
    /* Fresh stream: reset dead-backend streak so Win11 flaky Pa_IsStreamActive does not rebuild immediately. */
    if (stream_active_now) {
        g_rx_pa_stream_inactive_streak = 0U;
    }
#endif

    g_audio_start_inflight = 0;
    if (started_ok && g_audio == audio && dashcdg_desktop_audio_output_device_ready(audio)
#if DASHCDG_HAVE_PORTAUDIO
            && stream_active_now
#endif
    ) {
        g_audio_stream_started = 1;
        g_rx_force_full_preroll_start = 0;
        g_rx_audio_start_fail_log_ms = 0U;
    } else {
        const char *pa_detail = dashcdg_desktop_audio_last_stream_open_error();

        g_audio_stream_started = 0;
        if (g_rx_audio_start_fail_log_ms == 0U || now_ms - g_rx_audio_start_fail_log_ms >= 5000U) {
            if (pa_detail != NULL && pa_detail[0] != '\0') {
                RX_ERR( "[rx] audio: output device start failed: %s\n", pa_detail);
            } else {
                RX_ERR(
                        "[rx] audio: output device start failed (no driver detail — "
                        "WinMM build, stderr fully buffered, or early return before Pa_OpenStream)\n"
                );
            }
            g_rx_audio_start_fail_log_ms = now_ms;
        }
    }
    pthread_mutex_unlock(&g_receiver.mutex);
}

static int dashcdg_rx_handle_dead_audio_backend_locked(uint64_t now_ms) {
    int use_legacy_recovery_fallback = dashcdg_rx_should_use_legacy_recovery_fallback();
    uint64_t since_last_ts_advance_ms;
    uint64_t since_last_queue_success_ms;
    uint32_t buffered_ms;
    int backend_inactive = 0;

    if (g_audio == NULL || !g_audio_stream_started) {
        return 0;
    }
#if DASHCDG_HAVE_PORTAUDIO
    /*
     * playback_running can remain set while the host stream is already stopped/aborted
     * (PortAudio inactive). Without this, RX never hits dead-backend recovery.
     * Debounce inactive: single-tick gaps are common on WASAPI — do not rebuild until inactive persists.
     */
    if (g_audio->stream != NULL) {
        if (Pa_IsStreamActive(g_audio->stream) == 1) {
            g_rx_pa_stream_inactive_streak = 0U;
        } else {
            if (g_rx_pa_stream_inactive_streak < 100000U) {
                g_rx_pa_stream_inactive_streak++;
            }
            backend_inactive = (g_rx_pa_stream_inactive_streak >= DASHCDG_RX_PA_DEAD_BACKEND_MIN_STREAK);
        }
    }
#endif
    if (!backend_inactive && dashcdg_desktop_audio_is_running(g_audio)) {
        return 0;
    }
    /*
     * Ignore dead-backend probing until at least one queue success after (re)configure/start.
     * Before first queued frame, Win11/WASAPI inactive blips are common and recovery would churn.
     */
    if (g_receiver.last_audio_queue_success_local_ms == 0U) {
        return 0;
    }

    /*
     * Guard against rebuild thrash on transient backend state flips. If DAC timestamp is still
     * advancing or queue writes succeeded recently, keep the current stream and let regular
     * underrun/zero-buffer logic decide. This is especially important on Win10/11 where
     * Pa_IsStreamActive may briefly report inactive around host hiccups.
     */
    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    since_last_ts_advance_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_timestamp_advance_local_ms);
    since_last_queue_success_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_queue_success_local_ms);
    if (backend_inactive &&
            buffered_ms > 0U &&
            since_last_ts_advance_ms < DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS &&
            since_last_queue_success_ms < DASHCDG_RX_BUFFERED_SILENT_STALL_RECOVER_MS) {
        return 0;
    }
    if (backend_inactive &&
            dashcdg_desktop_audio_is_running(g_audio) &&
            since_last_ts_advance_ms < DASHCDG_RX_PA_DEAD_BACKEND_MIN_STALE_MS &&
            since_last_queue_success_ms < DASHCDG_RX_PA_DEAD_BACKEND_MIN_STALE_MS) {
        return 0;
    }
    if (since_last_ts_advance_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS &&
            since_last_queue_success_ms < DASHCDG_RX_HOST_UNDERRUN_RECOVER_MIN_STALE_MS) {
        return 0;
    }

    g_receiver.audio_last_stall_recover_local_ms = now_ms;
    if (use_legacy_recovery_fallback) {
        dashcdg_rx_reprime_audio_after_host_underrun_locked(&g_receiver);
    } else {
        /*
         * Light reset (flush ring + decoder) is not enough on some Win10/11 + PortAudio paths: the
         * host stream can be inactive while counters/HUD still look plausible. Match the D-toggle
         * re-enable path: stop, re-init the PCM ring, reopen the device, re-prime jitter/decoders.
         */
        dashcdg_rx_rebuild_audio_decode_path_locked(&g_receiver, now_ms);
    }
    return 1;
}

static void dashcdg_rx_maybe_log_audio_watchdog_locked(uint64_t now_ms) {
    static uint64_t s_last_watchdog_log_ms = 0U;
    uint32_t buffered_ms;
    uint64_t since_q_ms;
    uint64_t since_ts_ms;
    int running_flag;
    int output_ready;
    char line[256];

    if (g_audio == NULL) {
        return;
    }
    if (s_last_watchdog_log_ms != 0U && now_ms - s_last_watchdog_log_ms < 2000U) {
        return;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    since_q_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_queue_success_local_ms);
    since_ts_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_timestamp_advance_local_ms);
    running_flag = dashcdg_desktop_audio_is_running(g_audio);
    output_ready = dashcdg_desktop_audio_output_device_ready(g_audio);

    snprintf(
            line,
            sizeof(line),
            "[rx] audio-watchdog started=%u inflight=%u running=%u ready=%u buf=%ums since_q=%" DASHCDG_RX_PRIu64 "ms since_ts=%" DASHCDG_RX_PRIu64 "ms pending=%u/%u underruns=%" DASHCDG_RX_PRIu64,
            (unsigned int) g_audio_stream_started,
            (unsigned int) g_audio_start_inflight,
            (unsigned int) running_flag,
            (unsigned int) output_ready,
            (unsigned int) buffered_ms,
            (uint64_t) since_q_ms,
            (uint64_t) since_ts_ms,
            (unsigned int) dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter),
            (unsigned int) dashcdg_rx_pending_cdg_count(&g_receiver),
            (uint64_t) (g_audio != NULL ? g_audio->stream_underrun_events : 0U)
    );
    line[sizeof(line) - 1U] = '\0';
    dashcdg_rx_async_stdout_line(line);
    s_last_watchdog_log_ms = now_ms;
}

static void *dashcdg_rx_media_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    char fault_lines[8][256];
    char recovery_diag[256];
    int use_legacy_recovery_fallback = dashcdg_rx_should_use_legacy_recovery_fallback();

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);
    while (!g_rx_shutdown_requested) {
        int should_start_audio = 0;
        uint64_t now_ms = dashcdg_clock_now_ms();
        size_t fault_line_count = 0U;
        const char *recovery_line = NULL;
        const char *recovery_cause = NULL;

        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_note_audio_timestamp_progress_locked(&g_receiver, now_ms);
        dashcdg_rx_maybe_log_audio_watchdog_locked(now_ms);
        if (dashcdg_rx_handle_dead_audio_backend_locked(now_ms)) {
            if (use_legacy_recovery_fallback) {
                recovery_line = "[rx] audio: detected stopped host stream; legacy-safe restart gating engaged";
            } else {
                recovery_line = "[rx] audio: detected stopped host stream; rebuilding live pipeline and restarting device";
            }
            recovery_cause = "dead_backend";
        }
        if (recovery_line == NULL &&
                dashcdg_rx_should_auto_recover_host_underrun_locked(&g_receiver, now_ms)) {
            g_receiver.audio_last_stall_recover_local_ms = now_ms;
            g_receiver.recovery_host_underrun_count++;
            dashcdg_rx_reprime_audio_after_host_underrun_locked(&g_receiver);
            recovery_line = "[rx] audio: re-priming after host underrun burst";
            recovery_cause = "host_underrun";
        }
        if (recovery_line == NULL &&
                dashcdg_rx_should_auto_recover_decode_stall_locked(&g_receiver, now_ms)) {
            g_receiver.audio_last_stall_recover_local_ms = now_ms;
            g_receiver.recovery_silent_stall_count++;
            dashcdg_rx_rebuild_audio_decode_path_locked(&g_receiver, now_ms);
            recovery_line = "[rx] audio: rebuilding decode/output path after stalled jitter drain";
            recovery_cause = "decode_stall";
        }
        if (recovery_line == NULL &&
                dashcdg_rx_should_force_post_track_recover_locked(&g_receiver, now_ms)) {
            g_receiver.audio_last_stall_recover_local_ms = now_ms;
            g_receiver.recovery_silent_stall_count++;
            dashcdg_rx_rebuild_audio_decode_path_locked(&g_receiver, now_ms);
            recovery_line = "[rx] audio: fast post-track recover (stale timestamp/queue progress)";
            recovery_cause = "post_track_stale";
        }
        if (recovery_line == NULL && dashcdg_rx_park_idle_audio_output_locked(&g_receiver, now_ms)) {
            g_receiver.source_idle_park_count++;
            recovery_line = "[rx] audio: source idle, parking output until packets resume";
            recovery_cause = "idle_park";
        }
        dashcdg_rx_drain_media_locked(&g_receiver, now_ms);
        dashcdg_rx_adapt_jitter_capacity_locked(&g_receiver, now_ms);
        if (dashcdg_rx_should_auto_recover_zero_buffer_locked(&g_receiver, now_ms)) {
            g_receiver.audio_last_stall_recover_local_ms = now_ms;
            g_receiver.recovery_zero_buffer_count++;
            if (use_legacy_recovery_fallback) {
                dashcdg_rx_reprime_audio_after_host_underrun_locked(&g_receiver);
                recovery_line = "[rx] audio: legacy-safe re-prime after stalled zero-buffer stream";
            } else {
                dashcdg_rx_rebuild_audio_decode_path_locked(&g_receiver, now_ms);
                recovery_line = "[rx] audio: auto-recovering stalled zero-buffer stream (full output reinit)";
            }
            recovery_cause = "zero_buffer";
        } else if (dashcdg_rx_should_auto_recover_buffered_silent_locked(&g_receiver, now_ms)) {
            g_receiver.audio_last_stall_recover_local_ms = now_ms;
            g_receiver.recovery_silent_stall_count++;
            if (use_legacy_recovery_fallback) {
                dashcdg_rx_reprime_audio_after_host_underrun_locked(&g_receiver);
                recovery_line = "[rx] audio: legacy-safe re-prime after buffered silent stall";
            } else {
                dashcdg_rx_rebuild_audio_decode_path_locked(&g_receiver, now_ms);
                recovery_line = "[rx] audio: auto-recovering buffered silent stream (full output reinit)";
            }
            recovery_cause = "buffered_silent";
        }
        if (recovery_cause != NULL) {
            uint32_t buf_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
            uint64_t since_q_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_queue_success_local_ms);
            uint64_t since_ts_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_audio_timestamp_advance_local_ms);
            uint64_t since_dg_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_datagram_local_ms);
            size_t pending_audio = dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter);
            size_t pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
            snprintf(
                    recovery_diag,
                    sizeof(recovery_diag),
                    "[rx] audio-recovery cause=%s buf=%ums pending=%u/%u since_q=%" DASHCDG_RX_PRIu64 "ms since_ts=%" DASHCDG_RX_PRIu64 "ms since_dg=%" DASHCDG_RX_PRIu64 "ms started=%u inflight=%u",
                    recovery_cause,
                    (unsigned int) buf_ms,
                    (unsigned int) pending_audio,
                    (unsigned int) pending_cdg,
                    (uint64_t) since_q_ms,
                    (uint64_t) since_ts_ms,
                    (uint64_t) since_dg_ms,
                    (unsigned int) g_audio_stream_started,
                    (unsigned int) g_audio_start_inflight
            );
            recovery_diag[sizeof(recovery_diag) - 1U] = '\0';
        } else {
            recovery_diag[0] = '\0';
        }
        fault_line_count = dashcdg_rx_collect_fault_lines_locked(
                &g_receiver,
                now_ms,
                fault_lines,
                sizeof(fault_lines) / sizeof(fault_lines[0])
        );
        should_start_audio = dashcdg_rx_claim_audio_start_locked(now_ms);
        if (g_rx_last_render_snapshot_local_ms == 0U ||
                now_ms - g_rx_last_render_snapshot_local_ms >= dashcdg_rx_render_snapshot_interval_ms()) {
            dashcdg_rx_publish_render_snapshot_locked(now_ms);
            g_rx_last_render_snapshot_local_ms = now_ms;
        }
        dashcdg_rx_maybe_send_v4_stats_locked(now_ms);
        pthread_mutex_unlock(&g_receiver.mutex);

        if (recovery_line != NULL) {
            dashcdg_rx_async_stdout_line(recovery_line);
            if (recovery_diag[0] != '\0') {
                dashcdg_rx_async_stdout_line(recovery_diag);
            }
        }
        dashcdg_rx_emit_fault_lines(fault_lines, fault_line_count);

        if (should_start_audio) {
            dashcdg_rx_start_audio_async();
        }

        dashcdg_sleep_ms(10);
    }

    return NULL;
}

static void dashcdg_rx_emit_status_summary(void) {
    char hud_line_a[256];
    char hud_line_b[256];
    uint64_t now_ms = dashcdg_clock_now_ms();

    pthread_mutex_lock(&g_receiver.mutex);
    dashcdg_rx_fill_hud_lines_locked(
            now_ms,
            hud_line_a,
            sizeof(hud_line_a),
            hud_line_b,
            sizeof(hud_line_b),
            NULL,
            NULL
    );
    pthread_mutex_unlock(&g_receiver.mutex);

    dashcdg_rx_async_stdout_line(hud_line_a);
    dashcdg_rx_async_stdout_line(hud_line_b);
}

/* Windows snprintf + I64u in format strings confuses -Wformat; HUD only needs 32-bit ms. */
static unsigned int dashcdg_rx_hud_ms_display_u32(uint64_t ms) {
    if (ms >= (uint64_t) UINT_MAX) {
        return UINT_MAX;
    }
    return (unsigned int) ms;
}

static uint32_t dashcdg_rx_hud_round_u32_10ms(uint32_t ms) {
    return (ms + 5U) / 10U * 10U;
}

static int dashcdg_rx_hud_round_int_10ms(int v) {
    if (v >= 0) {
        return (v + 5) / 10 * 10;
    }
    return (v - 5) / 10 * 10;
}

static void dashcdg_rx_fill_hud_lines_locked(
        uint64_t local_now_ms,
        char *hud_line_a,
        size_t hud_line_a_size,
        char *hud_line_b,
        size_t hud_line_b_size,
        uint32_t *hud_line_a_rgb,
        uint32_t *hud_line_b_rgb
) {
    uint32_t hud_prefix_bytes = 0;
    uint64_t hud_since_last_dg_ms = 0;
    uint64_t hud_stall_ms = 0;
    uint64_t clock_hold_ms = 0;
    size_t pending_audio = 0;
    size_t pending_cdg = 0;
    size_t audio_repairable = 0;
    size_t cdg_repairable = 0;
    char audio_gate[64];
    char render_gate[64];
    int muted = g_audio != NULL ? dashcdg_desktop_audio_is_muted(g_audio) : g_audio_muted;
    uint32_t audio_buf_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    uint32_t target_buf_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    uint32_t host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    uint32_t target_total_ms = dashcdg_rx_audio_target_total_latency_ms_locked(&g_receiver);
    int hud_clock_skew_ms = 0;
    uint64_t hud_dac_playback_ms = 0U;
    uint64_t hud_snd_playback_ms = 0U;
    uint64_t repair_total = 0U;
    uint32_t repair_eff_x100 = 0U;
    char hud_skew_str[20];

    if (hud_line_a == NULL || hud_line_a_size == 0U || hud_line_b == NULL || hud_line_b_size == 0U) {
        return;
    }
    hud_line_a[0] = '\0';
    hud_line_b[0] = '\0';
    audio_gate[0] = '\0';
    render_gate[0] = '\0';
    if (hud_line_a_rgb != NULL) {
        *hud_line_a_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
    }
    if (hud_line_b_rgb != NULL) {
        *hud_line_b_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
        if (g_receiver.sync_group_mode != DASHCDG_TX_GROUP_SYNC_MODE_ACTIVE) {
            *hud_line_b_rgb = DASHCDG_RX_HUD_COLOR_WARN_RGB;
        }
        if (g_receiver.sync_group_phase_spread_ms >= DASHCDG_RX_HUD_GSPREAD_WARN_MS) {
            *hud_line_b_rgb = DASHCDG_RX_HUD_COLOR_WARN_RGB;
        }
        if (g_receiver.sync_group_phase_spread_ms >= DASHCDG_RX_HUD_GSPREAD_ERR_MS) {
            *hud_line_b_rgb = DASHCDG_RX_HUD_COLOR_ERR_RGB;
        }
    }

    pending_audio = dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter);
    pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    dashcdg_rx_collect_fec_group_stats(g_receiver.audio_fec_groups, NULL, NULL, &audio_repairable);
    dashcdg_rx_collect_fec_group_stats(g_receiver.cdg_fec_groups, NULL, NULL, &cdg_repairable);
    hud_prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    hud_since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_datagram_local_ms);
    hud_stall_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_progress_local_ms);
    clock_hold_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_clock_update_local_ms);

    dashcdg_rx_format_audio_gate_locked(&g_receiver, local_now_ms, audio_gate, sizeof(audio_gate));
    dashcdg_rx_format_render_gate_locked(&g_receiver, render_gate, sizeof(render_gate));
    repair_total = g_receiver.fec_cdg_recovered + g_receiver.fec_recovery_failures;
    if (repair_total > 0U) {
        repair_eff_x100 = (uint32_t)((g_receiver.fec_cdg_recovered * 10000ULL) / repair_total);
    }

    if (dashcdg_rx_local_audio_playback_now_locked(&hud_dac_playback_ms) &&
            dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &hud_snd_playback_ms)) {
        hud_clock_skew_ms = (int) ((int64_t) hud_snd_playback_ms - (int64_t) hud_dac_playback_ms);
        snprintf(hud_skew_str, sizeof(hud_skew_str), "%d", dashcdg_rx_hud_round_int_10ms(hud_clock_skew_ms));
    } else {
        snprintf(hud_skew_str, sizeof(hud_skew_str), "na");
    }

    snprintf(
            hud_line_a,
            hud_line_a_size,
            "v%u dg:%u parse:%u a:%u v:%u rec:%u/%u n:%u u:%u e:%u.%02u%%",
            (unsigned int) g_receiver.announced_transport_version,
            (unsigned int) (g_receiver.datagrams_received & 0xffffffffU),
            (unsigned int) (g_receiver.parse_failures & 0xffffffffU),
            (unsigned int) (g_receiver.v4_audio_chunk_packets & 0xffffffffU),
            (unsigned int) (g_receiver.v4_video_delta_packets & 0xffffffffU),
            (unsigned int) (g_receiver.fec_audio_recovered & 0xffffffffU),
            (unsigned int) (g_receiver.fec_cdg_recovered & 0xffffffffU),
            (unsigned int) (g_receiver.repair_nack_tx & 0xffffffffU),
            (unsigned int) (g_receiver.cdg_unrecoverable_groups & 0xffffffffU),
            (unsigned int) (repair_eff_x100 / 100U),
            (unsigned int) (repair_eff_x100 % 100U)
    );
    snprintf(
            hud_line_b,
            hud_line_b_size,
            "buf:%u/%u+%u=%u pend:%u/%u audio:%s gate:%s rend:%s sk:%s g:%s/%u/%u l:%u/%d",
            (unsigned int) dashcdg_rx_hud_round_u32_10ms(audio_buf_ms),
            (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_buf_ms),
            (unsigned int) dashcdg_rx_hud_round_u32_10ms(host_latency_ms),
            (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_total_ms),
            (unsigned int) pending_audio,
            (unsigned int) pending_cdg,
            g_audio_decode_disabled ? "DROP(decode-off)" : (muted ? "mute" : "on"),
            audio_gate,
            render_gate,
            hud_skew_str,
            dashcdg_rx_group_sync_mode_label(g_receiver.sync_group_mode),
            (unsigned int) g_receiver.sync_group_target_latency_ms,
            (unsigned int) g_receiver.sync_group_phase_spread_ms,
            (unsigned int) g_receiver.sync_leader_instance_id_low16,
            (int) g_receiver.sync_leader_trim_bias_ppm
    );
    (void)hud_prefix_bytes;
    (void)hud_since_last_dg_ms;
    (void)hud_stall_ms;
    (void)clock_hold_ms;
    (void)audio_repairable;
    (void)cdg_repairable;
    hud_line_a[hud_line_a_size - 1U] = '\0';
    hud_line_b[hud_line_b_size - 1U] = '\0';
}

static int dashcdg_rx_claim_audio_start_locked(uint64_t local_now_ms) {
    uint32_t buffered_ms;
    uint32_t required_ms;

    if (!g_receiver.network_audio_enabled || g_audio_decode_disabled || g_audio_stream_started || g_audio_start_inflight ||
            g_audio == NULL) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    required_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    if (!g_rx_force_full_preroll_start) {
        uint32_t fm = (uint32_t) (g_receiver.announced_audio_frame_ms != 0U ? g_receiver.announced_audio_frame_ms : 20U);
        uint32_t frame_window_ms = fm * 6U;

        if (frame_window_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
            frame_window_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
        }
        if (frame_window_ms > DASHCDG_RX_CLAIM_AUDIO_START_BUFFER_CAP_MS) {
            frame_window_ms = DASHCDG_RX_CLAIM_AUDIO_START_BUFFER_CAP_MS;
        }
        if (required_ms > frame_window_ms) {
            required_ms = frame_window_ms;
        }
    }

    if (buffered_ms < required_ms) {
        return 0;
    }

    /*
     * RX-first/TX-later startup can briefly have valid preroll PCM before clock_sync/session anchor is
     * fully established. Do not deadlock audio start on have_clock/sender_playback in that window.
     */
    if (!g_receiver.have_clock || g_receiver.playback_base_sender_ms == 0U) {
        return 1;
    }
    if (!dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &(uint64_t){0U})) {
        return 1;
    }

    /*
     * Startup must not compare sender_playback_now against stream_base_timestamp_ms when the host
     * device is already open (PortAudio stream or WinMM ctx) but g_audio_stream_started is still 0.
     * Cold join never hits that combination (no device until start_stream); resume-after-unpause and
     * similar flush paths do — sender vs first-queued playback_ms can disagree until the clock catches
     * up, which wedged claim on wait-preroll indefinitely. Preroll fill + requiring
     * dashcdg_rx_sender_playback_now_locked keep startup on the shared sender timeline.
     *
     * g_audio_stream_started is set only after dashcdg_desktop_audio_start_stream succeeds so HUD,
     * CDG drain, and PortAudio refcount stay consistent when open/start fails.
     */
    return 1;
}

#if DASHCDG_RX_HAVE_GLUT

static void display(void) {
    struct dashcdg_rx_render_snapshot render_snapshot;
    struct dashcdg_cdg_state connecting_state;
    int have_render_snapshot = 0;
    int show_connecting = 0;
    int reconnecting = 0;
    uint64_t local_now_ms = dashcdg_clock_now_ms();
    char hud_line_a[256];
    char hud_line_b[256];
    uint32_t hud_color_a_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
    uint32_t hud_color_b_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
    int show_hud = 0;
    int lock_ok = 0;
    int in_move_guard = 0;

    if (g_rx_gl_display_active) {
        return;
    }
    g_rx_gl_display_active = 1;
    if (glutGetWindow() == 0 || g_renderer.program == 0U) {
        g_rx_gl_display_active = 0;
        return;
    }
    if (g_rx_gl_resize_pause_until_ms != 0U && local_now_ms < g_rx_gl_resize_pause_until_ms) {
        if (!g_rx_gl_move_guard_logged) {
            RX_ERR("[rx-gl] move-guard active (resize-pause)\n");
            g_rx_gl_move_guard_logged = 1;
        }
        g_rx_gl_display_active = 0;
        return;
    }
    in_move_guard = (g_rx_gl_move_guard_until_ms != 0U && local_now_ms < g_rx_gl_move_guard_until_ms);
    if (in_move_guard) {
        if (!g_rx_gl_move_guard_logged) {
            RX_ERR("[rx-gl] move-guard active (rapid resize callbacks)\n");
            g_rx_gl_move_guard_logged = 1;
        }
        /* Avoid touching GL at all while the window manager is in a volatile move/resize burst. */
        g_rx_gl_display_active = 0;
        return;
    }
    if (g_rx_gl_move_guard_logged) {
        RX_ERR("[rx-gl] move-guard cleared\n");
        g_rx_gl_move_guard_logged = 0;
    }
    if (g_rx_gl_pending_resize_w > 0 && g_rx_gl_pending_resize_h > 0) {
        dashcdg_gl_renderer_resize(&g_renderer, g_rx_gl_pending_resize_w, g_rx_gl_pending_resize_h);
        g_rx_gl_pending_resize_w = 0;
        g_rx_gl_pending_resize_h = 0;
    }

    pthread_mutex_lock(&g_render_mutex);
    if (g_render_snapshot.valid) {
        render_snapshot = g_render_snapshot;
        have_render_snapshot = 1;
    }
    pthread_mutex_unlock(&g_render_mutex);

    lock_ok = pthread_mutex_trylock(&g_receiver.mutex) == 0;
    if (lock_ok) {
        dashcdg_rx_connecting_overlay_decide_locked(local_now_ms, &show_connecting, &reconnecting);
        show_hud = g_hud_visible;
        if (show_hud) {
            dashcdg_rx_fill_hud_lines_locked(
                    local_now_ms,
                    hud_line_a,
                    sizeof(hud_line_a),
                    hud_line_b,
                    sizeof(hud_line_b),
                    &hud_color_a_rgb,
                    &hud_color_b_rgb
            );
        }
        pthread_mutex_unlock(&g_receiver.mutex);
    }

    if (show_connecting) {
        dashcdg_rx_render_connecting_state(&connecting_state, local_now_ms, reconnecting);
        dashcdg_gl_renderer_render(&g_renderer, &connecting_state);
    } else if (have_render_snapshot) {
        dashcdg_gl_renderer_render(&g_renderer, &render_snapshot.state);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (show_hud) {
        int win_w = glutGet(GLUT_WINDOW_WIDTH);
        int win_h = glutGet(GLUT_WINDOW_HEIGHT);

        if (win_w <= 0 || win_h <= 0) {
            glutSwapBuffers();
            g_rx_gl_display_active = 0;
            return;
        }
        glUseProgram(0);
        glDisable(GL_TEXTURE_2D);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, win_w, win_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glColor3f(
                (GLfloat) ((hud_color_a_rgb >> 16U) & 0xffU) / 255.0f,
                (GLfloat) ((hud_color_a_rgb >> 8U) & 0xffU) / 255.0f,
                (GLfloat) (hud_color_a_rgb & 0xffU) / 255.0f
        );
        glRasterPos2i(8, 18);
        for (const char *c = hud_line_a; *c != '\0'; ++c) {
            glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int) (unsigned char) *c);
        }
        glColor3f(
                (GLfloat) ((hud_color_b_rgb >> 16U) & 0xffU) / 255.0f,
                (GLfloat) ((hud_color_b_rgb >> 8U) & 0xffU) / 255.0f,
                (GLfloat) (hud_color_b_rgb & 0xffU) / 255.0f
        );
        glRasterPos2i(8, 34);
        for (const char *c = hud_line_b; *c != '\0'; ++c) {
            glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int) (unsigned char) *c);
        }

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    }

    glutSwapBuffers();
    g_rx_gl_display_active = 0;
}

static void rx_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;

    if (key == 'i' || key == 'I') {
        pthread_mutex_lock(&g_receiver.mutex);
        g_hud_visible = !g_hud_visible;
        RX_OUT( "[rx] HUD %s\n", g_hud_visible ? "enabled" : "hidden");
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (key == 'm' || key == 'M') {
        pthread_mutex_lock(&g_receiver.mutex);
        g_audio_muted = !g_audio_muted;
        if (g_audio != NULL) {
            dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        }
        RX_OUT( "[rx] audio %s\n", g_audio_muted ? "muted" : "unmuted");
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (key == 'd' || key == 'D') {
        dashcdg_rx_toggle_audio_decode_drop();
    } else if (key == 's' || key == 'S') {
        dashcdg_rx_emit_status_summary();
    }
}

static void dashcdg_rx_render_timer(int value) {
    uint64_t now_ms = dashcdg_clock_now_ms();
    (void) value;

    if (!g_rx_gl_display_active &&
            (g_rx_gl_resize_pause_until_ms == 0U || now_ms >= g_rx_gl_resize_pause_until_ms) &&
            (g_rx_gl_move_guard_until_ms == 0U || now_ms >= g_rx_gl_move_guard_until_ms)) {
        glutPostRedisplay();
    }
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_rx_render_timer, 0);
}

static void resize_callback(int width, int height) {
    uint64_t now_ms = dashcdg_clock_now_ms();

    g_rx_gl_pending_resize_w = width;
    g_rx_gl_pending_resize_h = height;
    g_rx_gl_resize_pause_until_ms = now_ms + 200U;
    if (g_rx_gl_last_resize_cb_ms != 0U && now_ms > g_rx_gl_last_resize_cb_ms &&
            now_ms - g_rx_gl_last_resize_cb_ms <= 120U) {
        g_rx_gl_move_guard_until_ms = now_ms + 350U;
    }
    g_rx_gl_last_resize_cb_ms = now_ms;
}

#endif /* DASHCDG_RX_HAVE_GLUT */

#ifdef _WIN32
static void dashcdg_rx_win32_gdi_on_key(void *user, unsigned vk, int down) {
    (void) user;

    if (!down) {
        return;
    }
    if (vk == 'I' || vk == 'i' || vk == 0x49) {
        pthread_mutex_lock(&g_receiver.mutex);
        g_hud_visible = !g_hud_visible;
        RX_OUT( "[rx] HUD %s\n", g_hud_visible ? "enabled" : "hidden");
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (vk == 'M' || vk == 'm' || vk == 0x4D) {
        pthread_mutex_lock(&g_receiver.mutex);
        g_audio_muted = !g_audio_muted;
        if (g_audio != NULL) {
            dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        }
        RX_OUT( "[rx] audio %s\n", g_audio_muted ? "muted" : "unmuted");
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (vk == 'D' || vk == 'd' || vk == 0x44) {
        dashcdg_rx_toggle_audio_decode_drop();
    } else if (vk == 'S' || vk == 's' || vk == 0x53) {
        dashcdg_rx_emit_status_summary();
    }
}

static void dashcdg_rx_run_win32_gdi_main(int argc, char **argv) {
    static uint8_t bgra_frame[DASHCDG_CDG_RGBA_BYTES];
    struct dashcdg_win32_gdi_view *view = NULL;
    const char *title = "dashcdg desktop receiver (GDI)";
    uint64_t next_frame_deadline_ms = 0U;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    (void) argc;
    if (argv != NULL && argv[0] != NULL) {
        title = argv[0];
    }

    if (!dashcdg_win32_gdi_view_create(
                &view,
                title,
                DASHCDG_VISIBLE_WIDTH * 4,
                DASHCDG_VISIBLE_HEIGHT * 4,
                dashcdg_rx_win32_gdi_on_key,
                NULL
        )) {
        RX_ERR( "[rx] failed to create Win32 GDI window\n");
        return;
    }
    while (dashcdg_win32_gdi_view_poll(view)) {
        struct dashcdg_rx_render_snapshot render_snapshot;
        struct dashcdg_cdg_state connecting_state;
        int have_render_snapshot = 0;
        int show_connecting = 0;
        int reconnecting = 0;
        uint64_t local_now_ms = dashcdg_clock_now_ms();
        char hud_line_a[256];
        char hud_line_b[256];
        uint32_t hud_color_a_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
        uint32_t hud_color_b_rgb = DASHCDG_RX_HUD_COLOR_OK_RGB;
        int show_hud = 0;

        dashcdg_frame_limit_wait(&next_frame_deadline_ms, DASHCDG_RENDER_FRAME_INTERVAL_MS);

        pthread_mutex_lock(&g_render_mutex);
        if (g_render_snapshot.valid) {
            render_snapshot = g_render_snapshot;
            have_render_snapshot = 1;
        }
        pthread_mutex_unlock(&g_render_mutex);

        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_connecting_overlay_decide_locked(local_now_ms, &show_connecting, &reconnecting);
        show_hud = g_hud_visible;
        if (show_hud) {
            dashcdg_rx_fill_hud_lines_locked(
                    local_now_ms,
                    hud_line_a,
                    sizeof(hud_line_a),
                    hud_line_b,
                    sizeof(hud_line_b),
                    &hud_color_a_rgb,
                    &hud_color_b_rgb
            );
        } else {
            hud_line_a[0] = '\0';
            hud_line_b[0] = '\0';
        }
        pthread_mutex_unlock(&g_receiver.mutex);

        if (show_connecting) {
            dashcdg_rx_render_connecting_state(&connecting_state, local_now_ms, reconnecting);
            dashcdg_cdg_state_to_bgra8(&connecting_state, bgra_frame);
        } else if (have_render_snapshot) {
            dashcdg_cdg_state_to_bgra8(&render_snapshot.state, bgra_frame);
        } else {
            memset(bgra_frame, 0, sizeof(bgra_frame));
        }

        if (!dashcdg_win32_gdi_view_present_bgra(
                view,
                bgra_frame,
                sizeof(bgra_frame),
                show_hud,
                hud_line_a,
                hud_line_b,
                hud_color_a_rgb,
                hud_color_b_rgb
        )) {
            break;
        }
    }

    dashcdg_win32_gdi_view_destroy(view);
}
#endif /* _WIN32 */

#if DASHCDG_RX_HAVE_GLUT

static int dashcdg_rx_run_glut_visual_loop(int *argc_ptr, char ***argv_ptr) {
    int argc = argc_ptr != NULL ? *argc_ptr : 0;
    char **argv = argv_ptr != NULL ? *argv_ptr : NULL;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    glutInit(&argc, argv);
    if (argc_ptr != NULL) {
        *argc_ptr = argc;
    }
    if (argv_ptr != NULL) {
        *argv_ptr = argv;
    }
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop receiver");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_renderer)) {
        RX_ERR( "failed to initialize OpenGL renderer\n");
#ifdef _WIN32
        RX_ERR( "[rx] falling back to Win32 GDI window\n");
        glutDestroyWindow(glutGetWindow());
        dashcdg_rx_run_win32_gdi_main(argc, argv);
        return 0;
#else
        glutDestroyWindow(glutGetWindow());
        return 1;
#endif
    }

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutKeyboardFunc(rx_keyboard);
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_rx_render_timer, 0);
    glutMainLoop();
    return 0;
}

#endif /* DASHCDG_RX_HAVE_GLUT */

#if DASHCDG_RX_HAVE_GLUT || defined(_WIN32)
static int dashcdg_rx_run_windowed_ui(int argc, char **argv) {
#if DASHCDG_RX_HAVE_GLUT && defined(_WIN32)
    if (g_rx_use_win_gdi) {
        dashcdg_rx_run_win32_gdi_main(argc, argv);
        return 0;
    }
#endif
#if DASHCDG_RX_HAVE_GLUT
    return dashcdg_rx_run_glut_visual_loop(&argc, &argv);
#elif defined(_WIN32)
    dashcdg_rx_run_win32_gdi_main(argc, argv);
    return 0;
#else
    (void) argc;
    (void) argv;
    RX_ERR( "windowed RX requires Windows or an OpenGL/GLUT build\n");
    return 1;
#endif
}
#endif /* DASHCDG_RX_HAVE_GLUT || _WIN32 */

int dashcdg_desktop_rx_main(int argc, char **argv) {
    pthread_t rx_thread;
    pthread_t rx_repair_thread;
    pthread_t media_thread;
    pthread_t stats_thread;
    pthread_t nack_thread;
    const char *positionals[2] = { NULL, NULL };
    int positional_index = 0;
    int positionals_consumed = 0;
    int port = DASHCDG_DEFAULT_NETWORK_PORT;
    int stats_port = DASHCDG_DEFAULT_NETWORK_STATS_PORT;
    int repair_port = DASHCDG_RX_DEFAULT_REPAIR_PORT;
    int repair_thread_started = 0;
    int help_i;

    dashcdg_rx_logger_boot(argv[0] != NULL ? argv[0] : "desktop-rx");

    for (help_i = 1; help_i < argc; ++help_i) {
        if (strcmp(argv[help_i], "--help") == 0 || strcmp(argv[help_i], "-h") == 0 || strcmp(argv[help_i], "-?") == 0) {
            dashcdg_rx_cli_print_help(argv[0] != NULL ? argv[0] : "desktop-rx");
            dashcdg_rx_logger_shutdown_if_needed();
            return 0;
        }
    }

    {
        char build_line[256];

        snprintf(build_line, sizeof(build_line), "[rx] build: %s (%s %s)\n", DASHCDG_BUILD_VERSION, __DATE__, __TIME__);
        build_line[sizeof(build_line) - 1U] = '\0';
        if (g_rx_logger_enabled) {
            dashcdg_async_logger_log_line(&g_rx_logger, DASHCDG_ASYNC_LOG_STDOUT, build_line);
        } else {
            (void) fputs(build_line, stdout);
            (void) fflush(stdout);
        }
    }
    dashcdg_rx_init_receiver_instance_id();

    g_endpoint_address = DASHCDG_DEFAULT_NETWORK_ADDRESS;
    memset(&g_endpoint_in_addr, 0, sizeof(g_endpoint_in_addr));
    g_endpoint_is_multicast = 0;
    g_endpoint_is_broadcast = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rx-stats-ms") == 0) {
            if (i + 1 >= argc) {
                RX_ERR( "%s: --rx-stats-ms requires a non-negative integer (0 = off)\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            if (!dashcdg_rx_is_number(argv[i])) {
                RX_ERR( "%s: --rx-stats-ms expects a non-negative integer\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            g_rx_stats_interval_ms = (uint32_t) strtoul(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--rx-stats-port") == 0) {
            if (i + 1 >= argc || !dashcdg_rx_is_number(argv[i + 1])) {
                RX_ERR("%s: --rx-stats-port requires <1..65535>\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            stats_port = atoi(argv[i]);
            if (stats_port <= 0 || stats_port > 65535) {
                RX_ERR("%s: --rx-stats-port requires <1..65535>\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--rx-repair-port") == 0) {
            if (i + 1 >= argc || !dashcdg_rx_is_number(argv[i + 1])) {
                RX_ERR("%s: --rx-repair-port requires <1..65535>\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            repair_port = atoi(argv[i]);
            if (repair_port <= 0 || repair_port > 65535) {
                RX_ERR("%s: --rx-repair-port requires <1..65535>\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--rx-av-sync-log-ms") == 0) {
            if (i + 1 >= argc) {
                RX_ERR( "%s: --rx-av-sync-log-ms requires a non-negative integer (0 = off)\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            if (!dashcdg_rx_is_number(argv[i])) {
                RX_ERR( "%s: --rx-av-sync-log-ms expects a non-negative integer\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            g_rx_av_sync_log_ms = (uint32_t) strtoul(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--rx-graphics-clock") == 0) {
            if (i + 1 >= argc) {
                RX_ERR( "%s: --rx-graphics-clock requires dac or sender\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            if (strcmp(argv[i], "dac") == 0) {
                g_rx_graphics_clock_sender = 0;
            } else if (strcmp(argv[i], "sender") == 0) {
                g_rx_graphics_clock_sender = 1;
            } else {
                RX_ERR( "%s: --rx-graphics-clock: expected dac or sender\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--rx-graphics-trim-ms") == 0) {
            if (i + 1 >= argc) {
                RX_ERR( "%s: --rx-graphics-trim-ms requires an integer\n", argv[0]);
                dashcdg_rx_logger_shutdown_if_needed();
                return 1;
            }
            ++i;
            g_rx_graphics_trim_ms = (int32_t) strtol(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--rx-drop-audio") == 0 || strcmp(argv[i], "--no-audio-decode") == 0) {
            g_audio_decode_disabled = 1;
            continue;
        }
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
            continue;
        }
#if DASHCDG_RX_HAVE_GLUT
#ifndef _WIN32
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            RX_ERR( "%s: --win-gdi / --gdi is only supported on Windows desktop builds\n", argv[0]);
            dashcdg_rx_logger_shutdown_if_needed();
            return 1;
        }
#else
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            g_rx_use_win_gdi = 1;
            continue;
        }
#endif
#elif defined(_WIN32)
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            continue;
        }
#endif

        if (positional_index >= 2) {
            dashcdg_rx_print_usage(argv[0]);
            dashcdg_rx_logger_shutdown_if_needed();
            return 1;
        }

        positionals[positional_index++] = argv[i];
    }

    if (positional_index > 0 && !dashcdg_rx_is_number(positionals[0])) {
        g_endpoint_address = positionals[0];
        positionals_consumed = 1;
    }

    if (positionals_consumed < positional_index && dashcdg_rx_is_number(positionals[positionals_consumed])) {
        port = atoi(positionals[positionals_consumed]);
        positionals_consumed++;
    }

    if (positionals_consumed != positional_index || port <= 0) {
        dashcdg_rx_print_usage(argv[0]);
        dashcdg_rx_logger_shutdown_if_needed();
        return 1;
    }
    g_rx_stats_port = stats_port;
    g_rx_repair_port = (repair_port > 0 && repair_port != port) ? repair_port : 0;
    RX_OUT("[rx] config: stats_ms=%u stats_port=%d repair_port=%d codec=%s hud=%s nack_cooldown=%ums\n",
           (unsigned int) g_rx_stats_interval_ms,
           g_rx_stats_port,
           g_rx_repair_port > 0 ? g_rx_repair_port : port,
           g_audio_decode_disabled ? "drop-audio" : "decode-audio",
           g_hud_visible ? "on" : "off",
           (unsigned int) DASHCDG_RX_REPAIR_NACK_COOLDOWN_MS);

#if DASHCDG_RX_HAVE_GLUT
    if (g_headless && g_rx_use_win_gdi) {
        RX_ERR( "%s: cannot combine --headless and --win-gdi\n", argv[0]);
        dashcdg_rx_logger_shutdown_if_needed();
        return 1;
    }
#endif

    if (!dashcdg_rx_parse_ipv4_address(g_endpoint_address, &g_endpoint_in_addr)) {
        RX_ERR( "invalid endpoint address: %s\n", g_endpoint_address);
        dashcdg_rx_logger_shutdown_if_needed();
        return 1;
    }

    g_endpoint_is_multicast = dashcdg_rx_ipv4_is_multicast(&g_endpoint_in_addr);
    g_endpoint_is_broadcast = dashcdg_rx_ipv4_is_broadcast(&g_endpoint_in_addr);

    {
        const char *listen_suffix;

        if (g_headless) {
            listen_suffix = " (headless stdout stats mode)";
        } else {
#if DASHCDG_RX_HAVE_GLUT
            listen_suffix = g_rx_use_win_gdi ?
                    " (GDI window; HUD hidden by default, press I/M/D/S as in GL mode)" :
                    " (windowed; HUD hidden by default, press I to toggle HUD, M to mute/unmute, D to drop audio decode, S for stats line to stdout)";
#else
            listen_suffix = " (GDI window; HUD hidden by default, press I/M/D/S as in GL mode)";
#endif
        }
        RX_OUT("[rx] listening on %s:%d (stats:%d repair:%d)%s\n", g_endpoint_address, port, g_rx_stats_port,
               g_rx_repair_port, listen_suffix);
    }
    if (g_audio_decode_disabled) {
        RX_OUT( "[rx] audio decode disabled at startup (dropping incoming audio packets)\n");
    }

    if (!dashcdg_net_init()) {
        RX_ERR( "failed to initialize network stack\n");
        dashcdg_rx_logger_shutdown_if_needed();
        return 1;
    }
    dashcdg_win32_process_timing_enable();

    dashcdg_rx_init_stats_sender(g_rx_stats_port);

    memset(&g_receiver, 0, sizeof(g_receiver));
    dashcdg_rx_jitter_heap_init(&g_receiver);
    pthread_mutex_init(&g_receiver.mutex, NULL);
    pthread_mutex_init(&g_render_mutex, NULL);
    pthread_mutex_init(&g_rx_nack_queue_mutex, NULL);
    pthread_cond_init(&g_rx_nack_queue_cond, NULL);
    g_rx_nack_queue_head = 0U;
    g_rx_nack_queue_tail = 0U;
    g_rx_nack_queue_count = 0U;
    g_rx_nack_queue_dropped = 0U;
    memset(&g_render_snapshot, 0, sizeof(g_render_snapshot));
    dashcdg_cdg_reader_init(&g_receiver.reader);
    dashcdg_media_clock_init(&g_receiver.sender_clock);
    g_audio = NULL;
    g_rx_shutdown_requested = 0;
    g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;

    pthread_create(&rx_thread, NULL, network_thread, &port);
    if (g_rx_repair_port > 0) {
        pthread_create(&rx_repair_thread, NULL, network_thread, &g_rx_repair_port);
        repair_thread_started = 1;
    }
    pthread_create(&media_thread, NULL, dashcdg_rx_media_thread_main, NULL);
    pthread_create(&stats_thread, NULL, dashcdg_rx_stats_thread_main, NULL);
    pthread_create(&nack_thread, NULL, dashcdg_rx_repair_nack_thread_main, NULL);

    if (g_headless) {
        pthread_join(rx_thread, NULL);
        if (repair_thread_started) {
            pthread_join(rx_repair_thread, NULL);
        }
        pthread_join(media_thread, NULL);
        pthread_join(stats_thread, NULL);
        pthread_join(nack_thread, NULL);
        receiver_state_reset(&g_receiver);
        dashcdg_rx_jitter_heap_shutdown(&g_receiver);
        pthread_cond_destroy(&g_rx_nack_queue_cond);
        pthread_mutex_destroy(&g_rx_nack_queue_mutex);
        dashcdg_rx_logger_shutdown_if_needed();
        return 0;
    }

#if DASHCDG_RX_HAVE_GLUT || defined(_WIN32)
    if (dashcdg_rx_run_windowed_ui(argc, argv) != 0) {
        dashcdg_rx_request_shutdown();
        pthread_join(rx_thread, NULL);
        if (repair_thread_started) {
            pthread_join(rx_repair_thread, NULL);
        }
        pthread_join(media_thread, NULL);
        pthread_join(stats_thread, NULL);
        pthread_join(nack_thread, NULL);
        dashcdg_net_cleanup();
        if (g_audio != NULL) {
            dashcdg_desktop_audio_stop_stream(g_audio);
            dashcdg_desktop_audio_free(g_audio);
        }
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        pthread_mutex_destroy(&g_render_mutex);
        pthread_mutex_destroy(&g_receiver.mutex);
        pthread_cond_destroy(&g_rx_nack_queue_cond);
        pthread_mutex_destroy(&g_rx_nack_queue_mutex);
        receiver_state_reset(&g_receiver);
        dashcdg_rx_jitter_heap_shutdown(&g_receiver);
        dashcdg_rx_logger_shutdown_if_needed();
        return 1;
    }
#else
    RX_ERR( "windowed RX requires a Win32 GDI-only build or OpenGL/GLUT\n");
    dashcdg_rx_request_shutdown();
    pthread_join(rx_thread, NULL);
    if (repair_thread_started) {
        pthread_join(rx_repair_thread, NULL);
    }
    pthread_join(media_thread, NULL);
    pthread_join(stats_thread, NULL);
    pthread_join(nack_thread, NULL);
    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    pthread_mutex_destroy(&g_render_mutex);
    pthread_mutex_destroy(&g_receiver.mutex);
    pthread_cond_destroy(&g_rx_nack_queue_cond);
    pthread_mutex_destroy(&g_rx_nack_queue_mutex);
    receiver_state_reset(&g_receiver);
    dashcdg_rx_jitter_heap_shutdown(&g_receiver);
    dashcdg_rx_logger_shutdown_if_needed();
    return 1;
#endif

    dashcdg_rx_request_shutdown();
    pthread_cond_broadcast(&g_rx_nack_queue_cond);
    pthread_join(rx_thread, NULL);
    if (repair_thread_started) {
        pthread_join(rx_repair_thread, NULL);
    }
    pthread_join(media_thread, NULL);
    pthread_join(stats_thread, NULL);
    pthread_join(nack_thread, NULL);
    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    pthread_mutex_destroy(&g_render_mutex);
    pthread_mutex_destroy(&g_receiver.mutex);
    pthread_cond_destroy(&g_rx_nack_queue_cond);
    pthread_mutex_destroy(&g_rx_nack_queue_mutex);
    receiver_state_reset(&g_receiver);
    dashcdg_rx_jitter_heap_shutdown(&g_receiver);
    dashcdg_rx_logger_shutdown_if_needed();
    return 0;
}
