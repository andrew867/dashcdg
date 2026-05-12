/*
 * v4 UDP multicast receiver task: parse wire packets, CDG jitter drain, raster to panel (band blit).
 */
#include "badge_rx.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#if defined(__has_include)
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#endif
#endif

#include "dashcdg/amr_codec.h"
#include "dashcdg/audio_jitter.h"
#include "dashcdg/cdg.h"
#include "dashcdg/cdg_batch_jitter.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_qos.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/pcm_rate_convert.h"
#include "dashcdg/protocol.h"

#include "badge_cdg_rgb565.h"
#include "badge_prefs.h"
#include "display_lvgl.h"
#include "platform_hw.h"
#if defined(CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT) && CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT
#include "badge_sp_blit_worker.h"
#endif

static const char *TAG = "badge_rx";

#define BADGE_RX_MCAST_ADDR "239.255.77.77"
#define BADGE_RX_PORT       24684
/* Same as desktop `DASHCDG_TX_DEFAULT_REPAIR_PORT`: v4 CDG FEC repair when TX uses split port. */
#define BADGE_RX_REPAIR_PORT 24686
/* Receiver stats multicast on same group, separate port so peers can observe each other. */
#define BADGE_RX_TX_STATS_PORT 24685
/* AMR-WB decode + vendor C stack + recv path: 8 KiB overflowed; keep headroom. */
#define BADGE_RX_STACK      12288
#define BADGE_RX_TASK_PRIO  6
/*
 * Dual-core: pin RX off the PRO core where Wi-Fi ISR/stack work is heavier so decode + `select`
 * compete less with LVGL (typically on the other core via esp_lvgl_port). No effect when unicore.
 */
#if !CONFIG_FREERTOS_UNICORE
#define BADGE_RX_TASK_CORE 1
#endif
/** Multicast + STA unicast **media** port: high-rate v4_audio + CDG on one wire; needs real lwIP SO_RCVBUF. */
#define BADGE_RX_RCVBUF_MEDIA (128 * 1024)
/** Stats / repair ports: low rate; smaller buffer saves internal RAM when SO_RCVBUF is honored. */
#define BADGE_RX_RCVBUF_CONTROL (40 * 1024)
#define BADGE_CDG_LATE_GRACE_MS 120U
/* Slightly looser than desktop default: badge clock + Wi-Fi jitter; tight grace + false skips = picket-fence audio. */
#define BADGE_RX_AUDIO_LATE_GRACE_MS 120U
#define BADGE_RX_STATS_INTERVAL_MS 2000U
/** When decode is audio-only, report v4_rx_stats to TX faster for tuning drain/jitter. */
#define BADGE_RX_STATS_INTERVAL_AUDIO_ONLY_MS 1000U
#define BADGE_RX_STATS_INTERVAL_STARTUP_MS 8000U
#define BADGE_RX_STATS_INTERVAL_RECOVERY_MS 5000U
#define BADGE_RX_STATS_SKIP_AFTER_ENOMEM_MS 4000U
#define BADGE_RX_AUDIO_RX_NO_DAC_WARN_MS 500U
#define BADGE_RX_AUDIO_RX_NO_DAC_WARN_REPEAT_MS 2000U
#define BADGE_RX_MEDIA_DUP_CACHE_SLOTS 128U
#define BADGE_RX_MEDIA_PATH_AUTO_DECIDE_MS 800U
#define BADGE_RX_MEDIA_PATH_AUTO_HOLD_MS 2200U
#define BADGE_RX_MEDIA_PATH_FALLBACK_IDLE_MS 300U
#define BADGE_RX_MEDIA_PATH_AUTO_DUP_TRIGGER 4U
/*
 * `select` must wake often enough to (1) run `badge_rx_drain_v4_audio` so WiFi gaps do not balloon
 * `ms_since_prior_audio_apply` into hole-SKIP spirals, and (2) keep ESP32 `dac_continuous` fed
 * (~10.7 ms per 512-byte chunk @ 48 kHz). 200 ms idle was far too long — pops + choppy drops.
 * Values: Kconfig `DASHCDG_BADGE_RX_SELECT_TIMEOUT_*` (T8) when present; else defaults until `sdkconfig` refresh.
 */
#if defined(CONFIG_DASHCDG_BADGE_RX_SELECT_TIMEOUT_MS)
#define BADGE_RX_SELECT_TIMEOUT_MS ((uint32_t)CONFIG_DASHCDG_BADGE_RX_SELECT_TIMEOUT_MS)
#else
#define BADGE_RX_SELECT_TIMEOUT_MS 12U
#endif
#if defined(CONFIG_DASHCDG_BADGE_RX_SELECT_TIMEOUT_AUDIO_ONLY_MS)
#define BADGE_RX_SELECT_TIMEOUT_AUDIO_ONLY_MS ((uint32_t)CONFIG_DASHCDG_BADGE_RX_SELECT_TIMEOUT_AUDIO_ONLY_MS)
#else
#define BADGE_RX_SELECT_TIMEOUT_AUDIO_ONLY_MS 6U
#endif
#if CONFIG_DASHCDG_BADGE_RX_PUMP_MUTEX_MS > 0
#define BADGE_RX_PUMP_MUTEX_TICKS pdMS_TO_TICKS(CONFIG_DASHCDG_BADGE_RX_PUMP_MUTEX_MS)
#else
#define BADGE_RX_PUMP_MUTEX_TICKS portMAX_DELAY
#endif
#if CONFIG_DASHCDG_BADGE_RX_REPAIR_MUTEX_MS > 0
#define BADGE_RX_REPAIR_MUTEX_TICKS pdMS_TO_TICKS(CONFIG_DASHCDG_BADGE_RX_REPAIR_MUTEX_MS)
#else
#define BADGE_RX_REPAIR_MUTEX_TICKS portMAX_DELAY
#endif
/** Max audio jitter drain steps per call (AMR 20 ms frames; catch up after a UDP burst). */
#define BADGE_RX_AUDIO_DRAIN_GUARD 32U
#define BADGE_RX_AUDIO_DRAIN_BUDGET_MIXED 6U
#define BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET 6U
/*
 * Karaoke (no CDG video path): we can spend a few more drain steps per v4_audio_chunk so the
 * jitter ring does not sit one burst behind the DAC clock — reduces AMR/opus picket-fencing when
 * Wi-Fi delivers 20 ms frames in clumps.
 */
#define BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET_KARAOKE 8U
/*
 * Video decode off: **must stay tiny** — packet-path work runs inside `handle_v4_audio_chunk` while `s_mtx`
 * is held on the RX task. We only `dashcdg_audio_jitter_insert` there; decode/PLC is deferred until after
 * `xSemaphoreGive` (see `s_pump_defer_audio_drain`). Large in-mutex drains (we briefly used 52–96) blocked
 * `recvfrom` long enough for lwIP to drop UDP → `d_wm` explodes, `jb` sits at 0, skip counters climb = constant
 * hash (looks like "codec is broken" but is RX self-starvation). Idle/post-burst paths use GUARD_AUDIO_ONLY.
 */
#define BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET_AUDIO_ONLY 8U
#define BADGE_RX_AUDIO_DRAIN_BUDGET_MIXED_AUDIO_ONLY          16U
/** Full idle/post-burst drain cap when audio-only (packet path uses smaller per-chunk budgets). */
#define BADGE_RX_AUDIO_DRAIN_GUARD_AUDIO_ONLY                 128U
#define BADGE_RX_AUDIO_PLC_FRAMES_PER_CALL                    2U
#define BADGE_RX_AUDIO_PLC_FRAMES_AUDIO_ONLY                  6U
/**
 * Deferred packet-path drain (`badge_rx_deferred_audio_drain_if_pending_locked`) may scale `max_steps`
 * up to this cap when the ring is deep. This runs **after** the prior datagram's `recvfrom` returned,
 * so it does not reintroduce the old "insert + huge drain under one mutex hold before recv" starvation,
 * but it does pull real backlog down instead of letting `occupied == capacity` (HUD pegged at a100).
 */
#define BADGE_RX_AUDIO_DRAIN_DEFERRED_MAX_STEPS_AUDIO_ONLY    32U
/** UART ESP_LOGI cadence for audio-only tuning (matches stats summary grep). */
#define BADGE_RX_UART_AUDIO_STATS_MS                          1000U
#if defined(DASHCDG_AUDIO_JITTER_HEAP_BACKED) && DASHCDG_AUDIO_JITTER_HEAP_BACKED
/*
 * `dashcdg_audio_jitter_init` allocates the slot+payload pool with calloc(30, …). Under A/V on,
 * v4 anchor reassembly and blit scratch can leave the heap too fragmented for ~9 KiB contiguous;
 * the struct alloc succeeds but capacity stays 0 — insert no-ops, drain never runs, user hears
 * only the amp click. Fall back to smaller pools (still enough for real-time playout at 20 ms).
 */
#define BADGE_RX_AUDIO_JB_SLOTS_FALLBACK_A 16U
#define BADGE_RX_AUDIO_JB_SLOTS_FALLBACK_B 8U
#endif
#define BADGE_RX_IGMP_REFRESH_INTERVAL_MS 15000U
#define BADGE_RX_IGMP_REFRESH_RETRY_INTERVAL_MS 2000U
/** While IGMP not joined, retry this often during the bootstrap window (cold boot / post-DHCP race). */
#define BADGE_RX_IGMP_BOOTSTRAP_RETRY_MS 150U
/** After dashcdg_badge_rx_start(), keep fast IGMP retries for this long if still not joined. */
#define BADGE_RX_IGMP_BOOTSTRAP_WINDOW_MS 6000U
#define BADGE_RX_IGMP_POST_START_DELAY_MS 25U
#define BADGE_RX_IGMP_POST_STAIP_EXTRA_DELAY_MS 100U
/**
 * STA unicast dups: **media** dup is required on many APs (mcast→unicast). Open it whenever missing
 * unless internal DRAM is critically fragmented (`CRITICAL` threshold).
 * **Stats/repair** dups are optional for RX; defer those when contiguous internal RAM is low so we
 * do not spike lwIP during IGMP/Wi-Fi timer pressure — never block reopening the media dup for that.
 */
/* Optional stats+repair STA dups; keep threshold low enough to open under Opus+LVGL (logs ~1–3 KiB largest). */
#define BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_OPTIONAL_BYTES 1536U
#define BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_MEDIA_CRITICAL 768U
/*
 * Do **not** auto-close multicast sockets because a port looks “idle”:
 * - Many APs deliver the same stream as STA unicast dup while multicast subscription still matters for
 *   path changes; silence can be bursty (sender idle, jitter buffer, Wi‑Fi PS).
 * - Re-opening + IGMP races DHCP/lwIP under memory pressure — the failure mode you debugged.
 * Stats multicast stays mandatory for HUD/diagnostics. Omitting **media** multicast is only safe as an
 * explicit user-visible mode (“STA duplicate only”) with docs, never heuristics — select() below
 * supports s_sock==-1 when a media RX path exists (e.g. ucast dup only).
 */
#define BADGE_RX_CDG_DRAIN_GUARD_BASE 8U
#define BADGE_RX_CDG_DRAIN_GUARD_MEDIUM 14U
#define BADGE_RX_CDG_DRAIN_GUARD_HIGH 22U
#define BADGE_RX_CDG_DRAIN_GUARD_EXTREME 32U
#define BADGE_RX_CDG_CATCHUP_LAG_MEDIUM_MS 400U
#define BADGE_RX_CDG_CATCHUP_LAG_HIGH_MS 900U
#define BADGE_RX_CDG_CATCHUP_LAG_EXTREME_MS 1800U
#if defined(ESP_PLATFORM)
/*
 * Multicast STA: a long wall-clock hold blocked draining even when the jitter ring was already deep
 * (sparse v4_clock_sync → have_clock late; MIN_HOLD still forced ~250 ms silence per session edge).
 */
#define BADGE_RX_AUDIO_START_MIN_HOLD_MS 80U
#else
#define BADGE_RX_AUDIO_START_MIN_HOLD_MS 250U
#endif
#define BADGE_RX_AUDIO_START_MAX_HOLD_MS 900U
#define BADGE_RX_AUDIO_START_BUFFER_CAP_MS 340U
/*
 * Keep this enabled so sender-side multicast diagnostics can see badge jitter/queue state.
 * Payload fields we cannot source accurately on ESP32 are left at zero.
 */
#define BADGE_RX_ENABLE_TX_V4_STATS 1
/** Keep this many jitter slots free; evict furthest-ahead batches when tighter. */
#define BADGE_RX_JB_HEADROOM 6U
#ifndef CONFIG_DASHCDG_BADGE_CDG_JB_HEADROOM_EXTRA
#define CONFIG_DASHCDG_BADGE_CDG_JB_HEADROOM_EXTRA 0
#endif
#define BADGE_RX_JB_HEADROOM_EFFECTIVE ((size_t)BADGE_RX_JB_HEADROOM + (size_t)CONFIG_DASHCDG_BADGE_CDG_JB_HEADROOM_EXTRA)
/*
 * If an anchor arrives slightly behind the CDG jitter cursor (reorder), still apply when within
 * this many subchannel packet indices — apply_snapshot_seek drops overlapped batches only.
 */
#define BADGE_RX_ANCHOR_APPLY_SLACK_PACKETS ((uint64_t)DASHCDG_MAX_CDG_BATCH_PACKETS * 8ULL)
/* When audio decode is off, preserve fewer free slots so video uses more of the ring. */
#define BADGE_RX_JB_HEADROOM_VIDEO_PRIORITY 1U
#define BADGE_RX_AUDIO_SLOTS_BALANCED 30U
#define BADGE_RX_AUDIO_SLOTS_PRIORITY 56U
#define BADGE_RX_AUDIO_SLOTS_MINIMAL 8U
#define BADGE_RX_VIDEO_SLOTS_BALANCED 14U
#define BADGE_RX_VIDEO_SLOTS_PRIORITY 24U
#define BADGE_RX_VIDEO_SLOTS_MINIMAL 6U
#define BADGE_RX_AUDIO_SLOTS_MAX 80U
#define BADGE_RX_VIDEO_SLOTS_MAX 128U
#define BADGE_RX_ADAPT_TUNE_MS 250U
#define BADGE_RX_ADAPT_STEP_SLOTS 4U
#define BADGE_RX_ADAPT_STEP_FAST_SLOTS 24U
#define BADGE_RX_ADAPT_OCC_PCT 65U
#define BADGE_RX_ADAPT_OCC_PCT_FAST 80U
#define BADGE_RX_ADAPT_SHRINK_OCC_PCT 20U
#define BADGE_RX_ADAPT_SHRINK_HOLDOFF_MS 12000U
#define BADGE_RX_ADAPT_BURST_OCC_PCT 40U
#define BADGE_RX_ADAPT_BURST_HOLD_MS 2200U
#define BADGE_RX_ADAPT_FAIL_COOLDOWN_MS 2500U
#define BADGE_RX_ADAPT_FAIL_STREAK_STEP_DOWN 3U
#define BADGE_RX_ADAPT_MIN_INTERNAL_FREE_BOTH_ON 12000U
#define BADGE_RX_ADAPT_MIN_INTERNAL_FREE_SINGLE_ON 4000U
#define BADGE_RX_ADAPT_MIN_LARGEST_BLOCK 4096U
#define BADGE_RX_ADAPT_AUDIO_HARD_MAX_SLOTS 192U
#define BADGE_RX_ADAPT_VIDEO_HARD_MAX_SLOTS 320U
#define BADGE_RX_WIRE_REORDER_WINDOW_BITS 16U
#define BADGE_RX_MEDIA_PATH_POLICY_BOTH 0U
#define BADGE_RX_MEDIA_PATH_POLICY_MCAST_PREFER 1U
#define BADGE_RX_MEDIA_PATH_POLICY_UCAST_PREFER 2U
#define BADGE_RX_MEDIA_PATH_POLICY_AUTO 3U
#define BADGE_RX_CLOCK_TICK_ESTIMATE_MS 50U
/*
 * v4_clock_sync carries `header.sender_time_ms` (TX wall). Re-anchoring every packet snaps
 * `s_mclk.offset_ms` to that sample → bursty/jittery sender stamps explode `sender_time_observed_ms`
 * in v4_rx_stats and confuse TX group-sync / latency (users see TX "fast-forward" or lockups when
 * a badge is on-net). Track skew with capped steps instead; first packet still locks via observe().
 */
#define BADGE_RX_MEDIA_CLOCK_OBSERVE_MAX_STEP_MS 10LL
#define BADGE_RX_V4_SESSION_REORDER_SENDER_SLACK_MS 100U
#define BADGE_RX_SYNC_LEADER_BIAS_STALE_MS 3000U
#define BADGE_RX_CDG_HARD_RESYNC_EVENT_MIN_MISS 8U
#define BADGE_RX_CDG_HARD_RESYNC_COOLDOWN_MS 450U
/*
 * Reported to TX in v4_rx_stats.host_output_latency_ms so group-sync phase math is not stuck at 0
 * for ESP32 (no PortAudio ring query). ~one I2S DMA period at 48 kHz mono.
 */
#define BADGE_RX_REPORTED_HOST_OUTPUT_LATENCY_MS 40U
/** ESP32 `dac_continuous` runs at a single rate; AMR 8/16 kHz PCM is up-sampled in software. */
#define BADGE_RX_KARAOKE_LINE_HZ 48000U
/*
 * When PLC / decode fails, mix stored mono with LFSR noise so loss sounds like satellite-radio
 * breakup (still audible) instead of digital silence.
 */
#define BADGE_RX_AUDIO_DEGRADE_LAST_WEIGHT  100
#define BADGE_RX_AUDIO_DEGRADE_NOISE_WEIGHT 0
/*
 * Stored mono snapshot for degraded playback (~13 ms @ 48 kHz). Kept < one full 20 ms frame to save
 * DRAM .bss on ESP32 (960 samples would overflow tight layouts by 280 bytes). 640 keeps enough history
 * for concealment while buying extra linker headroom when dram0_0_seg is near the edge.
 * 960 = one 20 ms frame @ 48 kHz (line rate after AMR→48k SRC); was 640 when AMR was pushed at native rate.
 */
#define BADGE_RX_AUDIO_LAST_MONO_CAP 960
/*
 * Heap PCM scratch ceiling (int16_t samples): worst case ~60 ms Opus @ 48 kHz stereo interleaved.
 * Actual allocation is `badge_rx_pcm_scratch_samples_need()` — AMR-NB/WB and narrow Opus use far less.
 */
#define BADGE_RX_PCM_SCRATCH_SAMPLES_MAX 5760U
#define BADGE_RX_TX_GROUP_SYNC_MODE_OFF 0U
#define BADGE_RX_TX_GROUP_SYNC_MODE_MEASURE 1U
#define BADGE_RX_TX_GROUP_SYNC_MODE_ACTIVE 2U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MAGIC 0xA0000000U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MAGIC_MASK 0xF0000000U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MODE_SHIFT 26U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MODE_MASK 0x3U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_TARGET_SHIFT 16U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_TARGET_MASK 0x3FFU
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_SHIFT 8U
#define BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_MASK 0xFFU

typedef enum {
    BADGE_RX_MEM_PROFILE_MINIMAL = 0,
    BADGE_RX_MEM_PROFILE_AUDIO_PRIORITY = 1,
    BADGE_RX_MEM_PROFILE_VIDEO_PRIORITY = 2,
    BADGE_RX_MEM_PROFILE_BALANCED = 3,
} badge_rx_memory_profile_t;
#define BADGE_RX_CDG_REPAIR_GROUP_SIZE 9U
#define BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX 144U
#define BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX (BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX + 1U)
#define BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX 8U
#define BADGE_RX_PRE_ANCHOR_DRAIN_GRACE_MS 1800U
#define BADGE_RX_PRE_ANCHOR_PALETTE_GRACE_MS 4200U
/*
 * Keep full-height blits for correctness. Partial-height pressure clipping produced visible banding/
 * stale lower scanlines (wrong palette stripes + bottom held color) under current overlay path.
 */
#define BADGE_RX_CDG_BLIT_PARTIAL_H DASHCDG_BADGE_RX_VISIBLE_H
/* SPI write settle gap per band: avoids reusing one scratch band while prior DMA transfer is still active. */
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
    uint8_t parity_present_mask;
    uint8_t parity_symbol_bytes;
    uint8_t member_present[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint16_t member_lengths[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint8_t member_payloads[BADGE_RX_CDG_REPAIR_GROUP_SIZE][BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX];
    uint32_t group_id;
    uint8_t parity_symbols[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX];
};

static TaskHandle_t s_rx_task;
static volatile int s_sock = -1;
static volatile int s_stats_sock = -1;
static volatile int s_repair_sock = -1;
/** Optional second RX path: bind same ports to STA IPv4 (OpenWrt mcast-to-unicast delivers here). */
static volatile int s_ucast_media_sock = -1;
static volatile int s_ucast_stats_sock = -1;
static volatile int s_ucast_repair_sock = -1;
/** Send-only: v4_rx_stats + repair-nack to stats port (isolates control from media RX socket). */
static volatile int s_uplink_sock = -1;
static volatile int s_run;
/** Monotonic ms: do not open optional ucast (stats/repair dups) until this time (startup defer + ENOMEM cool-down). */
static uint64_t s_ucast_optional_earliest_ms;

static SemaphoreHandle_t s_mtx;

/*
 * CDG + jitter: static .bss by default, or internal heap when CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP.
 * Band blit scratch is always heap (~7 KiB). v4 RLE anchors decode into `s_cdg`.
 * With heap CDG, dashcdg_badge_rx_stop() frees CDG/jitter/scratch to return RAM to the pool.
 */
#ifndef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
static struct dashcdg_cdg_state s_cdg_storage;
static struct dashcdg_cdg_batch_jitter_buffer s_jb_storage;
#endif
static struct dashcdg_cdg_state *s_cdg;
static struct dashcdg_cdg_batch_jitter_buffer *s_jb;
static struct dashcdg_media_clock s_mclk;

static struct dashcdg_audio_jitter_buffer *s_audio_jb;
/** Decode PCM staging (size `s_pcm_scratch_samples` int16_t samples). */
static int16_t *s_amr_pcm48_scratch;
static size_t s_pcm_scratch_samples;
/** AMC/NB (and non–48k Opus) → 48 kHz cubic staging — must not live on the RX stack (deep call chain). */
#define BADGE_RX_RESAMPLE_TO48_CAP 1920U
static int16_t s_badge_rx_resample_to48_staging[BADGE_RX_RESAMPLE_TO48_CAP];
static void *s_amr_wb_decoder;
static void *s_amr_nb_decoder;
static struct dashcdg_opus_decoder s_opus_decoder;
static uint16_t s_opus_decoder_sr;
static uint8_t s_opus_decoder_ch;
static uint8_t s_opus_decoder_frame_ms;
static uint8_t s_opus_decoder_ready;
/** TOC channel count from wire Opus packets; 0 = unknown (falls back to session). Not cleared on decoder recreation. */
static uint8_t s_opus_pkt_channels;
static uint8_t s_announced_audio_frame_ms;
static uint8_t s_announced_audio_codec_id;
static uint8_t s_announced_audio_profile_id;
static uint16_t s_announced_audio_sample_rate;
static uint8_t s_announced_audio_channels;
static uint8_t s_audio_decode_enabled = 1U;
static uint8_t s_video_decode_enabled = 1U;
/** NVS-tunable when CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK; else forced off at build. */
static volatile uint8_t s_repair_nack_enabled =
#if CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
        1U
#else
        0U
#endif
        ;
/** NVS-tunable: periodic v4_rx_stats to TX (default on). */
static volatile uint8_t s_v4_stats_tx_enabled = 1U;
static int s_audio_decode_primed;
static uint64_t s_last_audio_jitter_apply_local_ms;
/* Last sender-timeline playback_ms we actually pushed to DAC (for v4_rx_stats latency on TX). */
static uint32_t s_last_presented_audio_timestamp_ms;

static uint16_t *s_cdg_blit_scratch;

static uint64_t s_sync_local_ms;
static uint64_t s_sync_playback_ms;
static uint16_t s_announced_playout_delay_ms;

/**
 * Estimated media `playback_ms` at local monotonic `now_ms`, aligned with wire `playback_ms` tags.
 * `dashcdg_media_clock` is fed from `v4_clock_sync.playback_ms` (not `header.sender_time_ms`) so
 * jitter/CDG drain does not mix sender wall time with the audio/CDG playback timeline.
 */
static uint64_t badge_rx_estimated_playback_ms_at_local(uint64_t local_now_ms)
{
    int64_t p = dashcdg_media_clock_remote_now(&s_mclk, (int64_t)local_now_ms);

    if (p < 0) {
        return 0U;
    }
    return (uint64_t)p;
}

static dashcdg_badge_rx_stats_t s_stats;

#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
static void badge_rx_perf_note_mtx_pump_us(uint32_t us)
{
    if (us > s_stats.perf_mtx_pump_max_us) {
        s_stats.perf_mtx_pump_max_us = us;
    }
}

static void badge_rx_perf_note_mtx_overlay_rgb_us(uint32_t us)
{
    if (us > s_stats.perf_mtx_overlay_rgb_max_us) {
        s_stats.perf_mtx_overlay_rgb_max_us = us;
    }
}
#endif

void dashcdg_badge_rx_perf_note_sp_blit_band_us(uint32_t band_spi_us)
{
#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
    if (band_spi_us > s_stats.perf_sp_blit_band_max_us) {
        s_stats.perf_sp_blit_band_max_us = band_spi_us;
    }
#else
    (void)band_spi_us;
#endif
}

static int s_jitter_cdg_primed;
static uint64_t s_last_cdg_apply_local_ms;

/** Network byte order; source IP of last unicast v4 datagram (TX host). */
static uint32_t s_v4_tx_src_ipv4;
static uint64_t s_last_v4_stats_sent_ms;
/** Rate limit UART audio stats (`audio_only` + `audio_chop` delta lines). */
static uint64_t s_last_uart_audio_stat_ms;
/** After rx_start: one interval primes chop snapshots so d_* are per-window, not since boot. */
static uint8_t s_uart_audio_chop_warmed;
/** Max audio jitter occupied slots seen since last `audio_chop` line (drain-path sampled). */
static uint32_t s_uart_audio_chop_jb_peak;
/** Snapshots for `audio_chop` delta line (proves loss / skips / underrun rate per second). */
static struct {
    uint32_t skips;
    uint32_t dec_fail;
    uint32_t degraded;
    uint32_t dac_begin;
    uint32_t frames_out;
    uint32_t chunks_rx;
    uint64_t datagrams;
    uint64_t wire_missing;
    uint32_t wire_reorder;
    uint32_t audio_miss;
    uint32_t jb_evict;
    uint64_t jb_reorder;
    uint64_t jb_drop;
    uint32_t recovery_und;
    uint32_t recovery_zero;
    uint32_t recovery_stall;
    uint32_t idle_miss;
    uint32_t post_miss;
    uint32_t coop;
    uint32_t dac_dma_ok;
    uint32_t dac_dma_err;
} s_uart_audio_chop_prev;
static uint32_t s_v4_stats_suppressed;
static uint64_t s_last_select_enomem_local_ms;
static uint64_t s_last_igmp_refresh_local_ms;
static uint64_t s_igmp_bootstrap_until_local_ms;
static volatile uint8_t s_sta_got_ip_pending_resync;
static uint32_t s_rx_mcast_media_datagrams;
static uint32_t s_rx_ucast_media_datagrams;
static uint32_t s_media_sequence_duplicate_hits;
static uint64_t s_last_mcast_media_rx_ms;
static uint64_t s_last_ucast_media_rx_ms;
static uint64_t s_media_dup_cache[BADGE_RX_MEDIA_DUP_CACHE_SLOTS];
static uint8_t s_media_path_policy = BADGE_RX_MEDIA_PATH_POLICY_AUTO;
static uint8_t s_media_auto_prefer_path; /* 0 none, 1 mcast, 2 ucast */
static uint64_t s_media_auto_hold_until_ms;
static uint64_t s_media_auto_last_eval_ms;
static uint32_t s_media_auto_dup_last;
static uint32_t s_media_auto_mcast_last;
static uint32_t s_media_auto_ucast_last;
static uint32_t s_rx_stats_seq;
static uint32_t s_badge_receiver_instance_id;
static int s_badge_receiver_instance_inited;
static uint8_t s_wire_seq_inited;
static uint64_t s_wire_next_expected;
static uint64_t s_wire_seen_bitmap;
static uint8_t s_audio_seq_inited;
static uint32_t s_audio_next_expected;
static uint8_t s_cdg_seq_inited;
static uint32_t s_cdg_next_expected;
static uint8_t s_clock_pb_inited;
static uint64_t s_clock_last_playback_ms;
static uint8_t s_playback_paused;
static uint64_t s_active_session_start_ms;
static uint32_t s_active_asset_size;
static char s_active_song_id[DASHCDG_MAX_SONG_ID];
static uint64_t s_active_session_local_start_ms;
static uint64_t s_session_epoch_sender_floor_ms;
static int s_active_session_valid;
static uint8_t s_sync_group_mode;
static uint16_t s_sync_group_target_latency_ms;
static uint8_t s_sync_group_phase_spread_ms;
static uint64_t s_cdg_skip_hold_until_local_ms;
static uint16_t s_sync_leader_instance_id_low16;
static int16_t s_sync_leader_trim_bias_ppm;
static uint64_t s_sync_leader_last_update_local_ms;
static int32_t s_sync_drift_trim_ppm_ema;
static uint32_t s_recovery_zero_buffer_count;
static uint32_t s_recovery_silent_stall_count;
static uint32_t s_recovery_host_underrun_count;
/** Startup gate blocked with jb_occ>0: when stall began; for rate-limited ESP_LOGW diagnosis. */
static uint64_t s_audio_start_gate_stall_begin_ms;
static uint64_t s_audio_start_gate_stall_last_log_ms;
/** Last good mono PCM @ 48 kHz for degraded / concealment fallback (see BADGE_RX_AUDIO_LAST_MONO_CAP). */
static int16_t s_rx_last_mono48[BADGE_RX_AUDIO_LAST_MONO_CAP];
static uint16_t s_rx_last_mono48_len;
static uint8_t s_rx_last_mono48_valid;
static uint32_t s_audio_degrade_lfsr;
/** PLC/degraded streak: soften output so long bursts are less fatiguing than full-level repeat. */
static uint16_t s_audio_degrade_loss_run;
/**
 * When set, `handle_v4_audio_chunk` deferred `badge_rx_drain_v4_audio_budget` until after the RX
 * pump releases `s_mtx` so `recvfrom` can run before decode/PLC (reduces lwIP UDP overrun / d_wm).
 */
static uint8_t s_pump_defer_audio_drain;
static uint32_t s_pump_defer_audio_max_steps;
static uint32_t s_pump_defer_audio_max_plc;
static uint32_t s_source_idle_park_count;
static uint64_t s_source_idle_last_mark_ms;
static uint32_t s_audio_rx_since_dac_push;
static uint64_t s_audio_rx_since_dac_push_start_ms;
static uint64_t s_audio_rx_no_dac_last_warn_ms;
static uint32_t s_audio_rx_no_dac_warn_count;
static uint8_t s_last_audio_codec_inited;
static uint8_t s_last_audio_codec_id;
static uint8_t s_last_audio_profile_id;
static struct badge_rx_video_repair_group s_video_repair_groups[BADGE_RX_TRACKED_VIDEO_REPAIR_GROUPS];

/** Clip CDG panel blit to Y in [0, max); full frame when not under jitter pressure. */
static uint16_t s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
/** Runtime CDG jitter reserve; adjusted by decode mode (audio on/off). */
static size_t s_cdg_jb_headroom_slots = BADGE_RX_JB_HEADROOM_EFFECTIVE;
/** Last successfully locked v4 stats payload snapshot (used if mutex is busy). */
static struct dashcdg_v4_rx_stats_payload s_v4_stats_snapshot;
static uint8_t s_v4_stats_snapshot_valid;
static badge_rx_memory_profile_t s_memory_profile = BADGE_RX_MEM_PROFILE_BALANCED;
static uint32_t s_memory_profile_switches;
static uint32_t s_memory_profile_resize_failures;
static uint32_t s_memory_profile_resize_audio_dropped;
static uint32_t s_memory_profile_resize_video_dropped;
static uint32_t s_memory_profile_adaptive_grows;
static uint32_t s_memory_profile_adaptive_shrinks;
static uint64_t s_last_adapt_tune_ms;
static uint32_t s_last_adapt_jb_evict_rounds;
static uint8_t s_last_adapt_fast_mode;
static uint16_t s_last_adapt_audio_max_slots;
static uint16_t s_last_adapt_video_max_slots;
static uint64_t s_last_adapt_grow_ms;
static uint64_t s_last_adapt_resize_fail_ms;
static uint8_t s_adapt_resize_fail_streak;
static uint32_t s_last_adapt_budget_bytes;
static uint32_t s_last_adapt_budget_contig_bytes;
static uint32_t s_last_adapt_budget_audio_bytes;
static uint32_t s_last_adapt_budget_video_bytes;
static uint64_t s_last_adapt_burst_ms;

/** v4 VIDEO_ANCHOR chunk assembly (heap; wire total_bytes is RLE-compressed, bounded by protocol). */
static uint8_t *s_v4_anchor_asm_buf;
static uint32_t s_v4_anchor_asm_id;
static uint64_t s_v4_anchor_asm_packet_index;
static uint32_t s_v4_anchor_asm_total_bytes;
static size_t s_v4_anchor_asm_received_bytes;
static uint8_t s_v4_anchor_chunk_seen[BADGE_RX_V4_ANCHOR_CHUNK_COUNT];
static uint32_t s_cdg_snapshots_applied;
static uint8_t s_pre_anchor_palette_mask;

static void drain_cdg_to_idle(uint64_t local_now_ms);
static void badge_rx_opus_decoder_reset(void);
static void badge_rx_amr_decoder_reset(void);
static int badge_rx_ensure_audio_jitter(void);
static size_t badge_rx_pcm_scratch_samples_need(void);
static int badge_rx_ensure_pcm_scratch(void);
static int badge_rx_ensure_opus_decoder(uint8_t frame_ms, const uint8_t *opus_pkt, size_t opus_len);
static void badge_rx_free_audio_jitter(void);
static void badge_rx_drain_v4_audio(uint64_t local_now_ms);
static void badge_rx_drain_v4_audio_budget(uint64_t local_now_ms, uint32_t max_steps, uint32_t max_plc_frames);
static void badge_rx_deferred_audio_drain_if_pending_locked(uint64_t local_now_ms);
static void badge_rx_pump_flush_deferred_audio_drain(uint64_t local_now_ms);
static int badge_rx_push_mono_resampled_to_karaoke_line(
        uint64_t local_now_ms, const int16_t *pcm_native, size_t n_native, uint32_t native_hz);
static int badge_rx_sta_has_ipv4(esp_netif_ip_info_t *out_ip);
static void badge_rx_maybe_periodic_igmp_refresh(uint64_t now_ms);

static int badge_rx_is_stale_prior_session_media(const struct dashcdg_packet_view *view)
{
    uint64_t gate_ms;
    uint64_t sender_ms;
    uint64_t preroll_ms;

    if (view == NULL || !s_active_session_valid || s_active_session_start_ms == 0U) {
        return 0;
    }
    sender_ms = view->header.sender_time_ms;
    preroll_ms = (uint64_t)(s_announced_playout_delay_ms != 0U ? s_announced_playout_delay_ms : 500U);
    gate_ms = sender_ms + preroll_ms + BADGE_RX_V4_SESSION_REORDER_SENDER_SLACK_MS;

    if (gate_ms >= s_active_session_start_ms) {
        return 0;
    }
    if (s_session_epoch_sender_floor_ms != 0U &&
        sender_ms + BADGE_RX_V4_SESSION_REORDER_SENDER_SLACK_MS >= s_session_epoch_sender_floor_ms) {
        return 0;
    }
    return 1;
}

/**
 * Audio-only stale filter: do not use `sender+preroll >= session_start` (CDG/video path). That gate plus
 * a missing `s_session_epoch_sender_floor_ms` (e.g. SESSION_INFO loss / reorder) dropped every chunk
 * while `audio_missing_estimate` climbed. Rely on sender floor when set; otherwise accept.
 */
static int badge_rx_is_stale_prior_session_audio(const struct dashcdg_packet_view *view)
{
    uint64_t sender_ms;

    if (view == NULL || !s_active_session_valid || s_active_session_start_ms == 0U) {
        return 0;
    }
    sender_ms = view->header.sender_time_ms;
    if (s_session_epoch_sender_floor_ms == 0U) {
        return 0;
    }
    if (sender_ms + BADGE_RX_V4_SESSION_REORDER_SENDER_SLACK_MS >= s_session_epoch_sender_floor_ms) {
        return 0;
    }
    return 1;
}
static void handle_v4_audio_chunk(const struct dashcdg_packet_view *view, uint64_t local_now_ms);
static int badge_rx_reset_for_new_session_locked(uint64_t local_now_ms);
static void badge_rx_apply_memory_profile_locked(void);
static void badge_rx_adapt_jitter_capacity_locked_with_heap(uint64_t now_ms, uint32_t free_internal,
                                                            uint32_t largest_internal);
static void badge_rx_adapt_jitter_capacity_unblocked(uint64_t now_ms);
static void badge_rx_note_wire_sequence(uint64_t seq);
static void badge_rx_note_media_sequence_duplicate(uint64_t seq);
static int badge_rx_media_path_allow_fd(int pump_fd, uint64_t now_ms);
static void badge_rx_media_path_update_auto(uint64_t now_ms);
static void badge_rx_note_stream_media_sequence(uint32_t seq, uint8_t *inited, uint32_t *next_expected, uint32_t *missing_counter);

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

static void badge_rx_note_stream_media_sequence(uint32_t seq, uint8_t *inited, uint32_t *next_expected, uint32_t *missing_counter)
{
    if (inited == NULL || next_expected == NULL || missing_counter == NULL) {
        return;
    }
    if (!(*inited)) {
        *inited = 1U;
        *next_expected = seq + 1U;
        return;
    }
    if (seq >= *next_expected) {
        *missing_counter += (seq - *next_expected);
        *next_expected = seq + 1U;
    }
}

/*
 * Tracks `packet_header.sequence`, which the desktop TX advances **once for every datagram** on the
 * main media socket (audio, CDG, clock_sync, session_info, anchors, …). Wi-Fi multicast routinely
 * delivers those datagrams out of strict sequence order, so `wire_reorder_events` grows quickly even
 * when the session is healthy. That is **not** the same thing as `media_sequence` reorder inside
 * the audio jitter buffer — see `pl.reorder_events` in stats, which sums jitter-buffer counters to
 * match desktop RX semantics. This wire-level tracker remains useful for coarse gap / missing
 * estimates (`wire_missing_estimate`) on the same global sequence space.
 */
static void badge_rx_note_wire_sequence(uint64_t seq)
{
    if (!s_wire_seq_inited) {
        s_wire_seq_inited = 1U;
        s_wire_next_expected = seq + 1U;
        s_wire_seen_bitmap = 0ULL;
        return;
    }
    if (seq < s_wire_next_expected) {
        s_stats.wire_reorder_events++;
        return;
    }

    uint64_t diff = seq - s_wire_next_expected;
    if (diff == 0U) {
        s_wire_next_expected++;
        while ((s_wire_seen_bitmap & 1ULL) != 0ULL) {
            s_wire_seen_bitmap >>= 1U;
            s_wire_next_expected++;
        }
        return;
    }

    if (diff >= BADGE_RX_WIRE_REORDER_WINDOW_BITS) {
        uint64_t advance = diff - (uint64_t)BADGE_RX_WIRE_REORDER_WINDOW_BITS + 1U;
        while (advance-- > 0U) {
            if ((s_wire_seen_bitmap & 1ULL) == 0ULL) {
                s_stats.wire_missing_estimate++;
            }
            s_wire_seen_bitmap >>= 1U;
            s_wire_next_expected++;
        }
        diff = seq - s_wire_next_expected;
    }
    s_stats.wire_reorder_events++;
    s_wire_seen_bitmap |= (1ULL << (uint8_t)diff);
}

static void badge_rx_note_media_sequence_duplicate(uint64_t seq)
{
    uint32_t idx = (uint32_t)(seq % (uint64_t)BADGE_RX_MEDIA_DUP_CACHE_SLOTS);

    if (s_media_dup_cache[idx] == seq) {
        s_media_sequence_duplicate_hits++;
    } else {
        s_media_dup_cache[idx] = seq;
    }
}

static void badge_rx_media_path_update_auto(uint64_t now_ms)
{
    uint32_t dup_delta;
    uint32_t mcast_delta;
    uint32_t ucast_delta;

    if ((int)s_sock < 0 || (int)s_ucast_media_sock < 0) {
        s_media_auto_prefer_path = 0U;
        s_media_auto_hold_until_ms = 0U;
        return;
    }
    if (s_media_auto_last_eval_ms != 0U &&
            now_ms - s_media_auto_last_eval_ms < BADGE_RX_MEDIA_PATH_AUTO_DECIDE_MS) {
        return;
    }
    dup_delta = s_media_sequence_duplicate_hits - s_media_auto_dup_last;
    mcast_delta = s_rx_mcast_media_datagrams - s_media_auto_mcast_last;
    ucast_delta = s_rx_ucast_media_datagrams - s_media_auto_ucast_last;
    s_media_auto_last_eval_ms = now_ms;
    s_media_auto_dup_last = s_media_sequence_duplicate_hits;
    s_media_auto_mcast_last = s_rx_mcast_media_datagrams;
    s_media_auto_ucast_last = s_rx_ucast_media_datagrams;

    if (dup_delta >= BADGE_RX_MEDIA_PATH_AUTO_DUP_TRIGGER) {
        s_media_auto_prefer_path = (ucast_delta >= mcast_delta) ? 2U : 1U;
        s_media_auto_hold_until_ms = now_ms + BADGE_RX_MEDIA_PATH_AUTO_HOLD_MS;
    } else if (s_media_auto_hold_until_ms != 0U && now_ms > s_media_auto_hold_until_ms) {
        s_media_auto_prefer_path = 0U;
        s_media_auto_hold_until_ms = 0U;
    }
}

static int badge_rx_media_path_allow_fd(int pump_fd, uint64_t now_ms)
{
    if (pump_fd == (int)s_sock && (int)s_ucast_media_sock < 0) {
        return 1;
    }
    if (pump_fd == (int)s_ucast_media_sock && (int)s_sock < 0) {
        return 1;
    }
    switch (s_media_path_policy) {
    case BADGE_RX_MEDIA_PATH_POLICY_MCAST_PREFER:
        if (pump_fd == (int)s_ucast_media_sock &&
                s_last_mcast_media_rx_ms != 0U &&
                now_ms - s_last_mcast_media_rx_ms < BADGE_RX_MEDIA_PATH_FALLBACK_IDLE_MS) {
            return 0;
        }
        return 1;
    case BADGE_RX_MEDIA_PATH_POLICY_UCAST_PREFER:
        if (pump_fd == (int)s_sock &&
                s_last_ucast_media_rx_ms != 0U &&
                now_ms - s_last_ucast_media_rx_ms < BADGE_RX_MEDIA_PATH_FALLBACK_IDLE_MS) {
            return 0;
        }
        return 1;
    case BADGE_RX_MEDIA_PATH_POLICY_AUTO:
        badge_rx_media_path_update_auto(now_ms);
        if (s_media_auto_prefer_path == 2U &&
                pump_fd == (int)s_sock &&
                s_last_ucast_media_rx_ms != 0U &&
                now_ms - s_last_ucast_media_rx_ms < BADGE_RX_MEDIA_PATH_FALLBACK_IDLE_MS) {
            return 0;
        }
        if (s_media_auto_prefer_path == 1U &&
                pump_fd == (int)s_ucast_media_sock &&
                s_last_mcast_media_rx_ms != 0U &&
                now_ms - s_last_mcast_media_rx_ms < BADGE_RX_MEDIA_PATH_FALLBACK_IDLE_MS) {
            return 0;
        }
        return 1;
    case BADGE_RX_MEDIA_PATH_POLICY_BOTH:
    default:
        return 1;
    }
}

static uint8_t badge_rx_gf256_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0U;
    for (uint8_t i = 0U; i < 8U; ++i) {
        if ((b & 1U) != 0U) {
            p ^= a;
        }
        if ((a & 0x80U) != 0U) {
            a = (uint8_t)((a << 1U) ^ 0x1DU);
        } else {
            a <<= 1U;
        }
        b >>= 1U;
    }
    return p;
}

static uint8_t badge_rx_gf256_pow(uint8_t base, uint8_t exp)
{
    uint8_t out = 1U;
    while (exp-- > 0U) {
        out = badge_rx_gf256_mul(out, base);
    }
    return out;
}

static uint8_t badge_rx_gf256_inv(uint8_t v)
{
    if (v == 0U) {
        return 0U;
    }
    return badge_rx_gf256_pow(v, 254U);
}

static uint8_t badge_rx_video_repair_coeff(uint8_t member_index, uint8_t parity_index)
{
    return badge_rx_gf256_pow((uint8_t)(member_index + 1U), parity_index);
}

static int badge_rx_gf256_solve(uint8_t n,
                                const uint8_t a_in[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX],
                                const uint8_t b_in[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX],
                                uint8_t x_out[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX])
{
    uint8_t a[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t b[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t row = 0U;
    if (n == 0U || n > BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
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
            uint8_t inv = badge_rx_gf256_inv(a[row][row]);
            if (inv == 0U) {
                return 0;
            }
            for (uint8_t c = 0U; c < n; ++c) {
                a[row][c] = badge_rx_gf256_mul(a[row][c], inv);
            }
            b[row] = badge_rx_gf256_mul(b[row], inv);
        }
        for (uint8_t r = 0U; r < n; ++r) {
            if (r == row || a[r][row] == 0U) {
                continue;
            }
            {
                uint8_t factor = a[r][row];
                for (uint8_t c = 0U; c < n; ++c) {
                    a[r][c] ^= badge_rx_gf256_mul(factor, a[row][c]);
                }
                b[r] ^= badge_rx_gf256_mul(factor, b[row]);
            }
        }
        row++;
    }
    memcpy(x_out, b, n);
    return 1;
}

static void badge_rx_video_repair_make_symbol(const uint8_t *payload, uint16_t payload_len, uint8_t *symbol,
                                              uint8_t symbol_bytes)
{
    if (payload == NULL || symbol == NULL || symbol_bytes == 0U) {
        return;
    }
    memset(symbol, 0, symbol_bytes);
    symbol[0] = (payload_len > 255U) ? 255U : (uint8_t)payload_len;
    if (payload_len > 0U && symbol_bytes > 1U) {
        uint16_t copy = payload_len;
        if ((uint16_t)(symbol_bytes - 1U) < copy) {
            copy = (uint16_t)(symbol_bytes - 1U);
        }
        memcpy(symbol + 1U, payload, copy);
    }
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
        /*
         * Record identity so handle_v4_video_anchor does not call begin() on every chunk (malloc
         * storm + log flood). Chunks then hit s_v4_anchor_asm_buf==NULL and return until the next
         * anchor id or session reset.
         */
        ESP_LOGW(TAG, "v4 anchor asm malloc %u failed", (unsigned)total_bytes);
        s_v4_anchor_asm_id = anchor_id;
        s_v4_anchor_asm_packet_index = packet_index;
        s_v4_anchor_asm_total_bytes = total_bytes;
        s_v4_anchor_asm_received_bytes = 0U;
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
    if (!s_video_decode_enabled) {
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
        /*
         * First anchor must always be accepted: intro-style tracks often emit key clear/title
         * content very early, and if early deltas advanced jitter cursors we still want this
         * canonical canvas snapshot instead of rejecting it as "old".
         */
        return 1;
    }
    if (s_jb == NULL || !s_jb->initialized) {
        return 0;
    }
    if (anchor_packet_index < s_jb->next_packet_index) {
        uint64_t gap = s_jb->next_packet_index - anchor_packet_index;
        if (gap <= BADGE_RX_ANCHOR_APPLY_SLACK_PACKETS) {
            return 1;
        }
        s_stats.v4_anchor_rejected_behind++;
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
        s_pre_anchor_palette_mask = 0x3U;
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

/** Match desktop `dashcdg_net_set_dscp`: one DSCP for all badge UDP fds (ESP32 Wi‑Fi AMPDU / BA RAM). */
static void badge_rx_sock_set_dashcdg_dscp(int fd, const char *what)
{
    int tos = (int)((DASHCDG_NET_DSCP_DASHCDG_DEFAULT & 0x3FU) << 2);

    if (fd < 0) {
        return;
    }
    if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) {
        ESP_LOGW(TAG, "%s: IP_TOS (DSCP %u) errno=%d", what != NULL ? what : "sock", (unsigned)DASHCDG_NET_DSCP_DASHCDG_DEFAULT, errno);
    }
}

/** Outbound UDP to TX stats listener (unicast when TX source is known; else multicast). */
static int badge_rx_open_uplink_socket(void)
{
    int s;
    int reuse = 1;
    uint8_t ttl = 32;
    struct in_addr if_addr;

    memset(&if_addr, 0, sizeof(if_addr));
    {
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na != NULL) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                if_addr.s_addr = ipi.ip.addr;
            }
        }
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "uplink socket errno=%d", errno);
        return -1;
    }
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    badge_rx_sock_set_dashcdg_dscp(s, "uplink");
    if (if_addr.s_addr != 0) {
        if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr)) != 0) {
            ESP_LOGW(TAG, "uplink IP_MULTICAST_IF errno=%d (STA IP may not be ready yet)", errno);
        }
    }
    (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return s;
}

/** FD for v4_rx_stats + repair-nack only (never media 24684). */
static int badge_rx_control_tx_fd(void)
{
    if ((int)s_uplink_sock >= 0) {
        return (int)s_uplink_sock;
    }
    if ((int)s_stats_sock >= 0) {
        return (int)s_stats_sock;
    }
    /* After retiring multicast stats RX, STA-bound dup can still sendto the stats multicast group. */
    if ((int)s_ucast_stats_sock >= 0) {
        return (int)s_ucast_stats_sock;
    }
    return -1;
}

static int badge_rx_is_audio_only_decode(void)
{
    return (s_audio_decode_enabled != 0U && s_video_decode_enabled == 0U) ? 1 : 0;
}

/** Sample peak audio jitter depth for `audio_chop` jb_pk= (called from drain hot path). */
static void badge_rx_uart_touch_jb_peak_for_chop(void)
{
    if (s_audio_jb == NULL) {
        return;
    }
    {
        uint32_t o = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);

        if (o > s_uart_audio_chop_jb_peak) {
            s_uart_audio_chop_jb_peak = o;
        }
    }
}

static uint8_t badge_rx_effective_audio_codec_id(void);

/** UART `audio_chop` deltas assume monotonic counters; DAC/jitter teardown can reset them backward. */
static uint32_t badge_rx_uart_monotonic_delta_u32(uint32_t cur, uint32_t prev)
{
    return (cur >= prev) ? (cur - prev) : 0U;
}

static uint64_t badge_rx_uart_monotonic_delta_u64(uint64_t cur, uint64_t prev)
{
    return (cur >= prev) ? (cur - prev) : 0U;
}

/**
 * Periodic UART: `audio_only` cumulative line when karaoke is audio-only;
 * `audio_chop` delta line whenever audio decode is on (proves choppiness: d_skip / d_udp / jb_pk).
 */
static void badge_rx_maybe_uart_log_audio_stats(uint64_t now_ms)
{
    uint32_t occ;
    uint32_t cap;
    uint64_t ms_since_pcm;
    uint64_t iv_ms;
    dashcdg_badge_rx_stats_t snap;
    uint64_t jb_reorder = 0U;
    uint64_t jb_drop = 0U;
    uint8_t announced_codec;
    uint8_t last_codec;
    uint8_t eff_codec;

    if (s_audio_decode_enabled == 0U) {
        return;
    }
    if (s_last_uart_audio_stat_ms != 0U &&
            (now_ms - s_last_uart_audio_stat_ms) < (uint64_t)BADGE_RX_UART_AUDIO_STATS_MS) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(8)) != pdTRUE) {
        return;
    }
    iv_ms = (s_last_uart_audio_stat_ms != 0U) ? (now_ms - s_last_uart_audio_stat_ms)
                                                : (uint64_t)BADGE_RX_UART_AUDIO_STATS_MS;
    if (iv_ms > 60000ULL) {
        iv_ms = BADGE_RX_UART_AUDIO_STATS_MS;
    }
    s_last_uart_audio_stat_ms = now_ms;

    occ = 0U;
    cap = 0U;
    if (s_audio_jb != NULL) {
        occ = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
        cap = (uint32_t)dashcdg_audio_jitter_capacity(s_audio_jb);
        jb_reorder = s_audio_jb->reordered_packets;
        jb_drop = s_audio_jb->pending_drops;
    }
    ms_since_pcm = 0U;
    if (s_last_audio_jitter_apply_local_ms != 0U && now_ms > s_last_audio_jitter_apply_local_ms) {
        ms_since_pcm = now_ms - s_last_audio_jitter_apply_local_ms;
    }
    snap = s_stats;
    announced_codec = s_announced_audio_codec_id;
    last_codec = (s_last_audio_codec_inited != 0U) ? s_last_audio_codec_id : 0U;
    eff_codec = badge_rx_effective_audio_codec_id();
    xSemaphoreGive(s_mtx);

    if (badge_rx_is_audio_only_decode()) {
        uint32_t dac_dma_ok = 0U;
        uint32_t dac_dma_err = 0U;
        uint32_t dac_hz_eff = 0U;
        uint32_t dac_chunk_u8 = 0U;
        uint32_t expect_dma_per_s = 0U;

        dashcdg_platform_hw_karaoke_dac_get_uart_health(&dac_dma_ok, &dac_dma_err, &dac_hz_eff, &dac_chunk_u8, NULL);
        if (dac_chunk_u8 > 0U && dac_hz_eff > 0U) {
            expect_dma_per_s = dac_hz_eff / dac_chunk_u8;
        }
        ESP_LOGI(TAG,
                 "audio_only a_rx=%lu a_out=%lu jb=%lu/%lu skip=%lu deg=%lu dec_fail=%lu dac_begin_fail=%lu "
                 "profile=%u clock=%d primed=%d ms_since_pcm=%llu mtx_idle_miss=%lu mtx_post_miss=%lu lvgl_coop=%lu "
                 "rx_mtx_to=%lu",
                 (unsigned long)snap.v4_audio_chunk_rx, (unsigned long)snap.v4_audio_frames_out, (unsigned long)occ,
                 (unsigned long)cap, (unsigned long)snap.v4_audio_jitter_skip_events,
                 (unsigned long)snap.v4_audio_degraded_push, (unsigned long)snap.v4_audio_decode_fail,
                 (unsigned long)snap.v4_audio_dac_begin_fail, (unsigned)s_memory_profile, (int)snap.have_clock,
                 s_audio_decode_primed, (unsigned long long)ms_since_pcm,
                 (unsigned long)snap.rx_mtx_idle_drain_miss, (unsigned long)snap.rx_mtx_postburst_drain_miss,
                 (unsigned long)snap.lvgl_coop_audio_drains, (unsigned long)snap.rx_mtx_pump_timeouts);
        /*
         * Proof line (grep AUDIO_UART_PROOF): dac_dma_err must stay 0; d_dma_ok/s ~ expect_dma_per_s when sound plays.
         * If dac_dma_ok frozen while a_rx climbs → decode/DAC path stuck; if dac_dma_err > 0 → ESP-IDF DMA reject.
         */
        ESP_LOGI(TAG,
                 "AUDIO_UART_PROOF dac_dma_ok=%lu dac_dma_err=%lu hz_eff=%lu chunk_u8=%lu expect_dma_per_s=%lu "
                 "vol_pct=%u",
                 (unsigned long)dac_dma_ok, (unsigned long)dac_dma_err, (unsigned long)dac_hz_eff,
                 (unsigned long)dac_chunk_u8, (unsigned long)expect_dma_per_s,
                 (unsigned)dashcdg_platform_hw_get_karaoke_output_volume_pct());
    }

    {
        wifi_ap_record_t ap;
        int rssi = -127;
        uint32_t heap_i;
        uint32_t jb_pk;
        int a_only = badge_rx_is_audio_only_decode();

        heap_i = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        jb_pk = s_uart_audio_chop_jb_peak;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }

        if (s_uart_audio_chop_warmed == 0U) {
            s_uart_audio_chop_prev.skips = snap.v4_audio_jitter_skip_events;
            s_uart_audio_chop_prev.dec_fail = snap.v4_audio_decode_fail;
            s_uart_audio_chop_prev.degraded = snap.v4_audio_degraded_push;
            s_uart_audio_chop_prev.dac_begin = snap.v4_audio_dac_begin_fail;
            s_uart_audio_chop_prev.frames_out = snap.v4_audio_frames_out;
            s_uart_audio_chop_prev.chunks_rx = snap.v4_audio_chunk_rx;
            s_uart_audio_chop_prev.datagrams = snap.datagrams;
            s_uart_audio_chop_prev.wire_missing = snap.wire_missing_estimate;
            s_uart_audio_chop_prev.wire_reorder = snap.wire_reorder_events;
            s_uart_audio_chop_prev.audio_miss = snap.audio_missing_estimate;
            s_uart_audio_chop_prev.jb_evict = snap.jb_evict_rounds;
            s_uart_audio_chop_prev.recovery_und = s_recovery_host_underrun_count;
            s_uart_audio_chop_prev.recovery_zero = s_recovery_zero_buffer_count;
            s_uart_audio_chop_prev.recovery_stall = s_recovery_silent_stall_count;
            s_uart_audio_chop_prev.jb_reorder = jb_reorder;
            s_uart_audio_chop_prev.jb_drop = jb_drop;
            s_uart_audio_chop_prev.idle_miss = snap.rx_mtx_idle_drain_miss;
            s_uart_audio_chop_prev.post_miss = snap.rx_mtx_postburst_drain_miss;
            s_uart_audio_chop_prev.coop = snap.lvgl_coop_audio_drains;
            dashcdg_platform_hw_karaoke_dac_get_uart_health(&s_uart_audio_chop_prev.dac_dma_ok,
                                                            &s_uart_audio_chop_prev.dac_dma_err, NULL, NULL, NULL);
            s_uart_audio_chop_warmed = 1U;
        } else {
            uint32_t d_skip = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_jitter_skip_events,
                                                                s_uart_audio_chop_prev.skips);
            uint32_t d_dec = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_decode_fail,
                                                               s_uart_audio_chop_prev.dec_fail);
            uint32_t d_deg = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_degraded_push,
                                                               s_uart_audio_chop_prev.degraded);
            uint32_t d_dac = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_dac_begin_fail,
                                                               s_uart_audio_chop_prev.dac_begin);
            uint32_t d_out = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_frames_out,
                                                               s_uart_audio_chop_prev.frames_out);
            uint32_t d_rx = badge_rx_uart_monotonic_delta_u32(snap.v4_audio_chunk_rx,
                                                              s_uart_audio_chop_prev.chunks_rx);
            uint64_t d_udp = badge_rx_uart_monotonic_delta_u64(snap.datagrams, s_uart_audio_chop_prev.datagrams);
            uint64_t d_wm =
                    badge_rx_uart_monotonic_delta_u64(snap.wire_missing_estimate, s_uart_audio_chop_prev.wire_missing);
            uint32_t d_wr = badge_rx_uart_monotonic_delta_u32(snap.wire_reorder_events,
                                                              s_uart_audio_chop_prev.wire_reorder);
            uint32_t d_am = badge_rx_uart_monotonic_delta_u32(snap.audio_missing_estimate,
                                                              s_uart_audio_chop_prev.audio_miss);
            uint32_t d_jbe = badge_rx_uart_monotonic_delta_u32(snap.jb_evict_rounds, s_uart_audio_chop_prev.jb_evict);
            uint32_t d_ru = badge_rx_uart_monotonic_delta_u32(s_recovery_host_underrun_count,
                                                              s_uart_audio_chop_prev.recovery_und);
            uint32_t d_rz = badge_rx_uart_monotonic_delta_u32(s_recovery_zero_buffer_count,
                                                              s_uart_audio_chop_prev.recovery_zero);
            uint32_t d_rs = badge_rx_uart_monotonic_delta_u32(s_recovery_silent_stall_count,
                                                              s_uart_audio_chop_prev.recovery_stall);
            uint64_t d_jbr = badge_rx_uart_monotonic_delta_u64(jb_reorder, s_uart_audio_chop_prev.jb_reorder);
            uint64_t d_jbd = badge_rx_uart_monotonic_delta_u64(jb_drop, s_uart_audio_chop_prev.jb_drop);
            uint32_t d_imiss = badge_rx_uart_monotonic_delta_u32(snap.rx_mtx_idle_drain_miss,
                                                                 s_uart_audio_chop_prev.idle_miss);
            uint32_t d_pmiss = badge_rx_uart_monotonic_delta_u32(snap.rx_mtx_postburst_drain_miss,
                                                                 s_uart_audio_chop_prev.post_miss);
            uint32_t d_coop = badge_rx_uart_monotonic_delta_u32(snap.lvgl_coop_audio_drains,
                                                                s_uart_audio_chop_prev.coop);
            uint32_t dac_cur_ok = 0U;
            uint32_t dac_cur_err = 0U;
            uint32_t d_dma_ok = 0U;
            uint32_t d_dma_err = 0U;

            dashcdg_platform_hw_karaoke_dac_get_uart_health(&dac_cur_ok, &dac_cur_err, NULL, NULL, NULL);
            d_dma_ok = badge_rx_uart_monotonic_delta_u32(dac_cur_ok, s_uart_audio_chop_prev.dac_dma_ok);
            d_dma_err = badge_rx_uart_monotonic_delta_u32(dac_cur_err, s_uart_audio_chop_prev.dac_dma_err);

            ESP_LOGI(
                    TAG,
                    "audio_chop iv_ms=%llu jb_pk=%u jb=%u/%u d_skip=%u d_dec=%u d_deg=%u d_dac=%u d_out=%u d_rx=%u "
                    "d_udp=%llu d_wm=%llu d_wr=%u d_amiss=%u d_jbe=%u d_jbr=%llu d_jbd=%llu d_ru=%u d_rz=%u d_rs=%u "
                    "d_imiss=%u d_pmiss=%u d_coop=%u d_dma_ok=%u d_dma_err=%u codec_ann=%u codec_wire=%u eff=%u "
                    "ms_sf=%llu heap_i=%u rssi=%d mcast=%u ucast=%u dup=%u a_only=%d",
                    (unsigned long long)iv_ms,
                    (unsigned)jb_pk,
                    (unsigned)occ,
                    (unsigned)cap,
                    (unsigned)d_skip,
                    (unsigned)d_dec,
                    (unsigned)d_deg,
                    (unsigned)d_dac,
                    (unsigned)d_out,
                    (unsigned)d_rx,
                    (unsigned long long)d_udp,
                    (unsigned long long)d_wm,
                    (unsigned)d_wr,
                    (unsigned)d_am,
                    (unsigned)d_jbe,
                    (unsigned long long)d_jbr,
                    (unsigned long long)d_jbd,
                    (unsigned)d_ru,
                    (unsigned)d_rz,
                    (unsigned)d_rs,
                    (unsigned)d_imiss,
                    (unsigned)d_pmiss,
                    (unsigned)d_coop,
                    (unsigned)d_dma_ok,
                    (unsigned)d_dma_err,
                    (unsigned)(unsigned int)announced_codec,
                    (unsigned)(unsigned int)last_codec,
                    (unsigned)(unsigned int)eff_codec,
                    (unsigned long long)ms_since_pcm,
                    (unsigned)heap_i,
                    rssi,
                    (unsigned)snap.mcast_media_datagrams,
                    (unsigned)snap.ucast_media_datagrams,
                    (unsigned)snap.media_sequence_duplicate_hits,
                    a_only
            );
            s_uart_audio_chop_prev.skips = snap.v4_audio_jitter_skip_events;
            s_uart_audio_chop_prev.dec_fail = snap.v4_audio_decode_fail;
            s_uart_audio_chop_prev.degraded = snap.v4_audio_degraded_push;
            s_uart_audio_chop_prev.dac_begin = snap.v4_audio_dac_begin_fail;
            s_uart_audio_chop_prev.frames_out = snap.v4_audio_frames_out;
            s_uart_audio_chop_prev.chunks_rx = snap.v4_audio_chunk_rx;
            s_uart_audio_chop_prev.datagrams = snap.datagrams;
            s_uart_audio_chop_prev.wire_missing = snap.wire_missing_estimate;
            s_uart_audio_chop_prev.wire_reorder = snap.wire_reorder_events;
            s_uart_audio_chop_prev.audio_miss = snap.audio_missing_estimate;
            s_uart_audio_chop_prev.jb_evict = snap.jb_evict_rounds;
            s_uart_audio_chop_prev.recovery_und = s_recovery_host_underrun_count;
            s_uart_audio_chop_prev.recovery_zero = s_recovery_zero_buffer_count;
            s_uart_audio_chop_prev.recovery_stall = s_recovery_silent_stall_count;
            s_uart_audio_chop_prev.jb_reorder = jb_reorder;
            s_uart_audio_chop_prev.jb_drop = jb_drop;
            s_uart_audio_chop_prev.idle_miss = snap.rx_mtx_idle_drain_miss;
            s_uart_audio_chop_prev.post_miss = snap.rx_mtx_postburst_drain_miss;
            s_uart_audio_chop_prev.coop = snap.lvgl_coop_audio_drains;
            s_uart_audio_chop_prev.dac_dma_ok = dac_cur_ok;
            s_uart_audio_chop_prev.dac_dma_err = dac_cur_err;
        }
        s_uart_audio_chop_jb_peak = 0U;
    }
}

/** v4_rx_stats: primary session multicast so all receivers + TX see reports (timing convergence). */
static void badge_rx_fill_v4_rx_stats_mcast_dst(struct sockaddr_in *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons(BADGE_RX_TX_STATS_PORT);
    dst->sin_addr.s_addr = inet_addr(BADGE_RX_MCAST_ADDR);
}

/**
 * v4_repair_nack: unicast to last seen TX when known (no player fan-out); else dedicated NACK multicast
 * (TX joins DASHCDG_V4_REPAIR_NACK_MCAST_ADDR_STR on the stats port; not the primary stats group).
 */
#if CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
static void badge_rx_fill_v4_repair_nack_dst(struct sockaddr_in *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons(BADGE_RX_TX_STATS_PORT);
    if (s_v4_tx_src_ipv4 != 0U) {
        dst->sin_addr.s_addr = s_v4_tx_src_ipv4;
    } else {
        dst->sin_addr.s_addr = inet_addr(DASHCDG_V4_REPAIR_NACK_MCAST_ADDR_STR);
    }
}
#endif

static void badge_rx_maybe_send_v4_stats(uint64_t now_ms)
{
    uint32_t target_interval_ms = BADGE_RX_STATS_INTERVAL_MS;

    if (s_audio_decode_enabled != 0U && s_video_decode_enabled == 0U) {
        target_interval_ms = BADGE_RX_STATS_INTERVAL_AUDIO_ONLY_MS;
    }

    if (!BADGE_RX_ENABLE_TX_V4_STATS || s_v4_stats_tx_enabled == 0U) {
        return;
    }
    uint8_t pkt[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_rx_stats_payload pl;
    struct sockaddr_in dst;
    size_t sz;
    ssize_t sent;

    if (badge_rx_control_tx_fd() < 0) {
        return;
    }
    if (s_last_select_enomem_local_ms != 0U &&
            now_ms > s_last_select_enomem_local_ms &&
            (now_ms - s_last_select_enomem_local_ms) < BADGE_RX_STATS_SKIP_AFTER_ENOMEM_MS) {
        s_v4_stats_suppressed++;
        return;
    }
    if (s_last_v4_stats_sent_ms != 0U && (now_ms - s_last_v4_stats_sent_ms) < (uint64_t)target_interval_ms) {
        return;
    }

    badge_rx_ensure_receiver_instance_id();
    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(8)) == pdTRUE) {
        uint16_t preroll_ms = s_announced_playout_delay_ms;
        uint32_t audio_jb_pending = 0U;
        uint32_t audio_jb_capacity = 0U;
        uint32_t video_jb_pending = 0U;
        uint32_t video_jb_capacity = 0U;
        uint8_t overloaded = 0U;
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
        if (s_audio_jb != NULL) {
            audio_jb_pending = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
            audio_jb_capacity = (uint32_t)dashcdg_audio_jitter_capacity(s_audio_jb);
        }
        if (s_jb != NULL) {
            video_jb_pending = (uint32_t)dashcdg_cdg_batch_jitter_occupied_count(s_jb);
            video_jb_capacity = (uint32_t)dashcdg_cdg_batch_jitter_capacity(s_jb);
        }
        if (sess_ct > 0U) {
            startup_stage = DASHCDG_V4_RX_STARTUP_V4_METADATA;
        }
        if (have_clock) {
            startup_stage = DASHCDG_V4_RX_STARTUP_RUNNING;
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_HAVE_CLOCK;
            sender_obs_ms = badge_rx_estimated_playback_ms_at_local(now_ms);
            if (s_jb != NULL && s_jb->initialized) {
                int64_t lag = (int64_t)sender_obs_ms - (int64_t)s_jb->next_playback_ms;
                if (lag < 0) {
                    lag = 0;
                } else if (lag > (int64_t)INT32_MAX) {
                    lag = INT32_MAX;
                }
                s_stats.cdg_lag_ms = (int32_t)lag;
            } else {
                s_stats.cdg_lag_ms = 0;
            }
        } else {
            s_stats.cdg_lag_ms = 0;
        }
        if ((audio_jb_capacity > 0U && audio_jb_pending * 100U >= audio_jb_capacity * 80U) ||
                (video_jb_capacity > 0U && video_jb_pending * 100U >= video_jb_capacity * 85U) ||
                (s_stats.cdg_lag_ms >= (int32_t)BADGE_RX_CDG_CATCHUP_LAG_EXTREME_MS)) {
            overloaded = 1U;
        }
        if (s_playback_paused) {
            startup_stage = DASHCDG_V4_RX_STARTUP_PAUSED;
        }
        if (!have_clock || s_last_presented_audio_timestamp_ms == 0U || s_audio_decode_primed == 0U) {
            target_interval_ms = BADGE_RX_STATS_INTERVAL_STARTUP_MS;
        } else if (overloaded) {
            target_interval_ms = BADGE_RX_STATS_INTERVAL_RECOVERY_MS;
        }
        if (s_last_v4_stats_sent_ms != 0U && (now_ms - s_last_v4_stats_sent_ms) < (uint64_t)target_interval_ms) {
            s_v4_stats_suppressed++;
            xSemaphoreGive(s_mtx);
            return;
        }

        pl.report_seq = 0U;
        pl.wall_now_ms = now_ms;
        pl.sender_time_observed_ms = sender_obs_ms;
        pl.clock_offset_estimate_ms = skew_inited ? -skew_ms : 0;
        pl.playout_delay_ms_config = preroll_ms;
        pl.reserved0 = 0;
        pl.audio_buffer_ms = audio_jb_pending * (uint32_t)(s_announced_audio_frame_ms ? s_announced_audio_frame_ms : 20U);
        pl.audio_queue_pressure_events = s_stats.jb_evict_rounds;
        pl.fec_audio_recovered = 0U;
        {
            int32_t aj = skew_ms >= 0 ? skew_ms : -skew_ms;
            pl.jitter_rms_ms = (uint16_t)(aj > 65535 ? 65535 : aj);
        }
        if (s_stats.datagrams > 0U) {
            uint64_t lx100 = (s_stats.wire_missing_estimate * 10000ULL) / s_stats.datagrams;
            if (lx100 > 10000ULL) {
                lx100 = 10000ULL;
            }
            pl.loss_pct_x100 = (uint16_t)lx100;
        } else {
            pl.loss_pct_x100 = 0U;
        }
        pl.v4_codec_id = s_announced_audio_codec_id;
        pl.opus_bitrate_bps = 0U;
        pl.fec_decode_attempts = s_stats.v4_video_repair_rx_packets;
        pl.fec_recovery_failed = s_stats.v4_video_repair_failed;
        pl.media_datagrams_lost_estimated = (uint32_t)(s_stats.wire_missing_estimate > 0xffffffffULL
                                                           ? 0xffffffffU
                                                           : (uint32_t)s_stats.wire_missing_estimate);
        pl.cdg_fec_recovered = s_stats.v4_video_repair_recovered;
        pl.cdg_fec_failed = s_stats.v4_video_repair_failed;
        pl.jitter_p95_ms = pl.jitter_rms_ms;
        pl.jitter_max_ms = pl.jitter_rms_ms;
        {
            uint64_t rsum = 0ULL;

            if (s_audio_jb != NULL && s_audio_jb->initialized) {
                rsum += s_audio_jb->reordered_packets;
            }
            if (s_jb != NULL && s_jb->initialized) {
                rsum += s_jb->reordered_batches;
            }
            pl.reorder_events = rsum > 0xffffffffULL ? 0xffffffffU : (uint32_t)rsum;
        }
        pl.receiver_instance_id = s_badge_receiver_instance_id;
        pl.fec_group_size_observed = BADGE_RX_CDG_REPAIR_GROUP_SIZE;
        pl.presented_audio_timestamp_ms = s_last_presented_audio_timestamp_ms;
        pl.audio_buffer_target_ms = preroll_ms;
        pl.host_output_latency_ms = BADGE_RX_REPORTED_HOST_OUTPUT_LATENCY_MS;
        if (s_sync_group_target_latency_ms != 0U) {
            pl.target_total_latency_ms = s_sync_group_target_latency_ms;
        } else {
            pl.target_total_latency_ms = preroll_ms;
        }
        pl.startup_stage = startup_stage;
        {
            int32_t trim_ppm = s_sync_drift_trim_ppm_ema;
            if (s_sync_leader_last_update_local_ms != 0U &&
                now_ms > s_sync_leader_last_update_local_ms &&
                (now_ms - s_sync_leader_last_update_local_ms) <= BADGE_RX_SYNC_LEADER_BIAS_STALE_MS) {
                trim_ppm += (int32_t)s_sync_leader_trim_bias_ppm;
            }
            pl.drift_trim_ppm = trim_ppm;
        }
        pl.recovery_host_underrun_count = s_recovery_host_underrun_count;
        pl.recovery_zero_buffer_count = s_recovery_zero_buffer_count;
        pl.recovery_silent_stall_count = s_recovery_silent_stall_count;
        pl.source_idle_park_count = s_source_idle_park_count;
        if (s_audio_decode_enabled != 0U) {
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_NETWORK_AUDIO_ENABLED;
        }
        if (s_last_presented_audio_timestamp_ms != 0U) {
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_AUDIO_STREAM_STARTED;
        }
        if (s_playback_paused) {
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_LOADING_SCREEN_ACTIVE;
        }
        if (have_clock &&
            s_last_presented_audio_timestamp_ms > 0U &&
            pl.audio_buffer_ms > 0U &&
            startup_stage >= DASHCDG_V4_RX_STARTUP_RUNNING &&
            (startup_flags & DASHCDG_V4_RX_STARTUP_FLAG_RECOVERY_COOLDOWN) == 0U) {
            startup_flags |= DASHCDG_V4_RX_STARTUP_FLAG_LATENCY_CONFIDENT;
        }
        pl.startup_flags = startup_flags;
        pl.video_jb_pending_slots = video_jb_pending;
        pl.video_jb_next_packet_index = (s_jb != NULL) ? s_jb->next_packet_index : 0U;
        pl.v4_clock_rx_count = s_stats.v4_clock_count;
        pl.clock_skew_ema_ms = skew_ms;
        pl.ptp_offset_ema_us = 0;
        pl.heap_free_min_bytes = esp_get_minimum_free_heap_size();
        pl.wifi_rssi_dbm = 0;
        pl.ptp_mode = 1U;
        pl.stats_generation = 4U;
        pl.device_flags = (s_run ? 0x1U : 0U) |
                          (s_stats.cdg_heap_ok ? 0x2U : 0U) |
                          ((skew_inited ? 1U : 0U) << 2U) |
                          ((s_sync_group_phase_spread_ms == 0xffU ? 1U : 0U) << 3U) |
                          ((uint32_t)overloaded << 4U) |
                          ((uint32_t)(s_sync_group_mode & 0x3U) << 8U) |
                          ((uint32_t)(s_sync_group_phase_spread_ms) << 16U);
        s_v4_stats_snapshot = pl;
        s_v4_stats_snapshot_valid = 1U;
        xSemaphoreGive(s_mtx);
    } else if (s_v4_stats_snapshot_valid) {
        pl = s_v4_stats_snapshot;
        pl.wall_now_ms = now_ms;
    } else {
        return;
    }

    hdr.sequence = ++s_rx_stats_seq;
    hdr.sender_time_ms = now_ms;
    pl.report_seq = hdr.sequence;
    sz = dashcdg_protocol_serialize_v4_rx_stats(pkt, sizeof(pkt), &hdr, &pl);

    if (sz == 0U) {
        return;
    }

    badge_rx_fill_v4_rx_stats_mcast_dst(&dst);

    {
        int tx_fd = badge_rx_control_tx_fd();
        if (tx_fd < 0) {
            return;
        }
        sent = sendto(tx_fd, pkt, sz, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    if (sent == (ssize_t)sz) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            s_stats.v4_rx_stats_sent++;
            xSemaphoreGive(s_mtx);
        }
        s_last_v4_stats_sent_ms = now_ms;
    } else if (sent < 0) {
        ESP_LOGD(TAG, "v4 rx-stats sendto errno=%d", errno);
    }
}

#if CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK && CONFIG_DASHCDG_BADGE_REPAIR_NACK_THROTTLE
static uint32_t badge_rx_repair_nack_min_gap_ms(void)
{
    uint32_t base = (uint32_t)CONFIG_DASHCDG_BADGE_REPAIR_NACK_MIN_GAP_MS;
    const uint32_t max_gap = (uint32_t)CONFIG_DASHCDG_BADGE_REPAIR_NACK_MAX_GAP_MS;
    const int ref_db = CONFIG_DASHCDG_BADGE_REPAIR_NACK_RSSI_REF_DB;
    const int per_db = CONFIG_DASHCDG_BADGE_REPAIR_NACK_RSSI_EXTRA_MS_PER_DB;
    const int cap = CONFIG_DASHCDG_BADGE_REPAIR_NACK_RSSI_EXTRA_CAP_MS;
    wifi_ap_record_t ap;
    int extra = 0;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        int rssi = (int)ap.rssi;
        if (rssi < ref_db) {
            extra = (ref_db - rssi) * per_db;
            if (extra < 0) {
                extra = 0;
            }
            if (extra > cap) {
                extra = cap;
            }
        }
    }
    {
        uint32_t gap = base + (uint32_t)extra;
        if (gap > max_gap) {
            gap = max_gap;
        }
        return gap;
    }
}
#endif

#if CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
static void badge_rx_send_v4_repair_nack(uint64_t now_ms, uint32_t group_id, uint16_t observed_group_size, uint16_t missing_mask)
{
    static uint64_t s_last_nack_ms;
    static uint64_t s_last_nack_attempt_ms;
    static uint32_t s_last_nack_group_id;
    static uint16_t s_last_nack_mask;
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_repair_nack_payload pl;
    struct sockaddr_in dst;
    uint8_t pkt[DASHCDG_MAX_PACKET_SIZE];
    size_t sz;
    ssize_t sent;
    int fd;
    if (s_repair_nack_enabled == 0U) {
        return;
    }
    fd = badge_rx_control_tx_fd();
    if (fd < 0 || missing_mask == 0U) {
        return;
    }
    if (s_last_nack_group_id == group_id && s_last_nack_mask == missing_mask &&
        s_last_nack_ms != 0U && now_ms > s_last_nack_ms && now_ms - s_last_nack_ms < 120U) {
        return;
    }
#if CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK && CONFIG_DASHCDG_BADGE_REPAIR_NACK_THROTTLE
    {
        const uint32_t min_gap = badge_rx_repair_nack_min_gap_ms();
        if (min_gap > 0U && s_last_nack_attempt_ms != 0U && now_ms > s_last_nack_attempt_ms &&
            now_ms - s_last_nack_attempt_ms < min_gap) {
            s_stats.v4_repair_nack_throttled++;
            return;
        }
    }
#endif
    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));
    hdr.sequence = ++s_rx_stats_seq;
    hdr.sender_time_ms = now_ms;
    pl.stream_type = DASHCDG_STREAM_TYPE_CDG;
    pl.observed_group_size = observed_group_size;
    pl.group_id = group_id;
    pl.missing_member_mask = missing_mask;
    sz = dashcdg_protocol_serialize_v4_repair_nack(pkt, sizeof(pkt), &hdr, &pl);
    if (sz == 0U) {
        return;
    }
    s_stats.v4_repair_nack_attempt++;
    s_last_nack_attempt_ms = now_ms;
    badge_rx_fill_v4_repair_nack_dst(&dst);
    sent = sendto(fd, pkt, sz, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (sent == (ssize_t)sz) {
        s_stats.v4_repair_nack_tx++;
        s_last_nack_ms = now_ms;
        s_last_nack_group_id = group_id;
        s_last_nack_mask = missing_mask;
        ESP_LOGI(TAG, "repair-nack group=%u size=%u mask=0x%04x", (unsigned)group_id, (unsigned)observed_group_size,
                 (unsigned)missing_mask);
    } else {
        s_stats.v4_repair_nack_send_fail++;
        ESP_LOGD(TAG, "repair-nack sendto fail fd=%d sent=%d errno=%d", fd, (int)sent, errno);
    }
}
#else
static void badge_rx_send_v4_repair_nack(uint64_t now_ms, uint32_t group_id, uint16_t observed_group_size, uint16_t missing_mask)
{
    (void)now_ms;
    (void)group_id;
    (void)observed_group_size;
    (void)missing_mask;
}
#endif

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

/** Frees blit scratch + v4 anchor buffer; clears jitter. With heap CDG, frees CDG/jitter structs too. */
static void badge_rx_free_heap(void)
{
    badge_rx_v4_anchor_asm_reset();
    badge_rx_free_blit_scratch();
    if (s_jb != NULL) {
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
        dashcdg_cdg_batch_jitter_release(s_jb);
#else
        dashcdg_cdg_batch_jitter_clear(s_jb);
#endif
    }
#ifdef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
    if (s_jb != NULL) {
        heap_caps_free(s_jb);
        s_jb = NULL;
    }
    if (s_cdg != NULL) {
        heap_caps_free(s_cdg);
        s_cdg = NULL;
    }
#else
    s_jb = NULL;
    s_cdg = NULL;
#endif
}

/** Wire CDG + jitter (.bss or heap per Kconfig). */
static int badge_rx_ensure_heap(void)
{
    if (s_cdg != NULL && s_jb != NULL) {
        return 0;
    }
#ifdef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
    s_cdg = (struct dashcdg_cdg_state *)heap_caps_calloc(1, sizeof(*s_cdg), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_jb = (struct dashcdg_cdg_batch_jitter_buffer *)heap_caps_calloc(1, sizeof(*s_jb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_cdg == NULL || s_jb == NULL) {
        if (s_cdg != NULL) {
            heap_caps_free(s_cdg);
            s_cdg = NULL;
        }
        if (s_jb != NULL) {
            heap_caps_free(s_jb);
            s_jb = NULL;
        }
        ESP_LOGW(TAG, "CDG/jitter heap calloc failed");
        return -1;
    }
#else
    s_cdg = &s_cdg_storage;
    s_jb = &s_jb_storage;
#endif
    return 0;
}

/** After jitter drains or anchors, restore full 192px blit when queue is not under headroom pressure. */
static void badge_rx_cdg_blit_relax_pressure_clip(void)
{
    if (s_jb == NULL || !s_jb->initialized) {
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
        return;
    }
    if (dashcdg_cdg_batch_jitter_occupied_count(s_jb) + s_cdg_jb_headroom_slots <= dashcdg_cdg_batch_jitter_capacity(s_jb)) {
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    }
}

static uint8_t badge_rx_cdg_packet_palette_mask(const struct dashcdg_subchannel_packet *pkt)
{
    uint8_t insn;

    if (pkt == NULL || (pkt->command & 0x3FU) != 0x09U) {
        return 0U;
    }
    insn = (uint8_t)(pkt->instruction & 0x3FU);
    if (insn == DASHCDG_INSN_LOAD_COLOR_TABLE_00) {
        return 0x1U;
    }
    if (insn == DASHCDG_INSN_LOAD_COLOR_TABLE_08) {
        return 0x2U;
    }
    return 0U;
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
        s_pre_anchor_palette_mask |= badge_rx_cdg_packet_palette_mask(&pkt);
        (void)dashcdg_cdg_state_process_packet(s_cdg, &pkt);
    }
    dashcdg_cdg_batch_jitter_note_applied(s_jb, (struct dashcdg_cdg_batch_jitter_frame *)batch);
}

static void drain_cdg_to_idle(uint64_t local_now_ms)
{
    struct dashcdg_cdg_batch_jitter_drain_input din;
    struct dashcdg_cdg_batch_jitter_frame *batch = NULL;
    uint64_t miss = 0;
    uint32_t cdg_guard_limit = BADGE_RX_CDG_DRAIN_GUARD_BASE;

    if (s_cdg_snapshots_applied == 0U &&
        s_active_session_local_start_ms != 0U &&
        local_now_ms > s_active_session_local_start_ms &&
        (local_now_ms - s_active_session_local_start_ms) < BADGE_RX_PRE_ANCHOR_DRAIN_GRACE_MS) {
        /*
         * During a fresh session, prefer waiting briefly for the first anchor instead of applying
         * whatever mid-stream deltas arrived first. This reduces missed-clear/title-overlap artifacts.
         */
        return;
    }
    if (s_cdg_snapshots_applied == 0U &&
        s_pre_anchor_palette_mask != 0x3U &&
        s_active_session_local_start_ms != 0U &&
        local_now_ms > s_active_session_local_start_ms &&
        (local_now_ms - s_active_session_local_start_ms) < BADGE_RX_PRE_ANCHOR_PALETTE_GRACE_MS) {
        /* Keep startup frame stable until both color-table halves arrive or first anchor lands. */
        return;
    }
    int guard = 0;

    if (badge_rx_is_audio_only_decode()) {
        badge_rx_drain_v4_audio_budget(
                local_now_ms, BADGE_RX_AUDIO_DRAIN_BUDGET_MIXED_AUDIO_ONLY, BADGE_RX_AUDIO_PLC_FRAMES_AUDIO_ONLY);
    } else {
        badge_rx_drain_v4_audio_budget(local_now_ms, BADGE_RX_AUDIO_DRAIN_BUDGET_MIXED, BADGE_RX_AUDIO_PLC_FRAMES_PER_CALL);
    }

    if (s_jb == NULL || !s_jb->initialized) {
        return;
    }
    if (s_stats.have_clock) {
        uint64_t sender_playback_now_ms = badge_rx_estimated_playback_ms_at_local(local_now_ms);
        if (sender_playback_now_ms > s_jb->next_playback_ms) {
            uint64_t lag_ms = sender_playback_now_ms - s_jb->next_playback_ms;
            if (lag_ms >= BADGE_RX_CDG_CATCHUP_LAG_EXTREME_MS) {
                cdg_guard_limit = BADGE_RX_CDG_DRAIN_GUARD_EXTREME;
            } else if (lag_ms >= BADGE_RX_CDG_CATCHUP_LAG_HIGH_MS) {
                cdg_guard_limit = BADGE_RX_CDG_DRAIN_GUARD_HIGH;
            } else if (lag_ms >= BADGE_RX_CDG_CATCHUP_LAG_MEDIUM_MS) {
                cdg_guard_limit = BADGE_RX_CDG_DRAIN_GUARD_MEDIUM;
            }
        }
    }

    for (;;) {
        if (++guard > (int)cdg_guard_limit) {
            break;
        }
        memset(&din, 0, sizeof(din));
        if (s_stats.have_clock) {
            din.have_sender_playback = 1;
            din.sender_playback_now_ms = badge_rx_estimated_playback_ms_at_local(local_now_ms);
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
        if (s_cdg_skip_hold_until_local_ms != 0U && local_now_ms < s_cdg_skip_hold_until_local_ms) {
            din.late_gate = 0;
            din.ms_since_prior_cdg_apply = 0U;
            din.primed_decode = 0;
        }

        enum dashcdg_cdg_batch_drain_step step =
            dashcdg_cdg_batch_jitter_drain_step(s_jb, &din, &batch, &miss);
        if (step == DASHCDG_CDG_BATCH_DRAIN_SKIP) {
            s_stats.live_missing_skips += miss;
            if (miss >= BADGE_RX_CDG_HARD_RESYNC_EVENT_MIN_MISS) {
                uint64_t hold_ms = BADGE_RX_CDG_HARD_RESYNC_COOLDOWN_MS;
                if (s_announced_playout_delay_ms > hold_ms) {
                    hold_ms = s_announced_playout_delay_ms;
                }
                s_stats.cdg_hard_resync_events++;
                if (s_stats.cdg_hard_resync_packets > 0xffffffffU - (uint32_t)miss) {
                    s_stats.cdg_hard_resync_packets = 0xffffffffU;
                } else {
                    s_stats.cdg_hard_resync_packets += (uint32_t)miss;
                }
                s_cdg_skip_hold_until_local_ms = local_now_ms + hold_ms;
            }
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
    s_cdg_skip_hold_until_local_ms = 0U;
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
    if (s_video_decode_enabled) {
        badge_rx_try_alloc_cdg_jitter();
    }
}

static void badge_rx_opus_decoder_reset(void)
{
    dashcdg_opus_decoder_free(&s_opus_decoder);
    s_opus_decoder_ready = 0;
    s_opus_decoder_sr = 0U;
    s_opus_decoder_ch = 0U;
    s_opus_decoder_frame_ms = 0U;
}

static void badge_rx_amr_decoder_reset(void)
{
    badge_rx_opus_decoder_reset();
    s_opus_pkt_channels = 0U;
    if (s_amr_wb_decoder != NULL) {
        dashcdg_amr_wb_decoder_destroy(s_amr_wb_decoder);
        s_amr_wb_decoder = NULL;
    }
    if (s_amr_nb_decoder != NULL) {
        dashcdg_amr_nb_decoder_destroy(s_amr_nb_decoder);
        s_amr_nb_decoder = NULL;
    }
}

/** Drop AMR decoder heap only (Opus state unchanged). Frees RAM before (re)creating an Opus decoder. */
static void badge_rx_drop_amr_decoders(void)
{
    if (s_amr_wb_decoder != NULL) {
        dashcdg_amr_wb_decoder_destroy(s_amr_wb_decoder);
        s_amr_wb_decoder = NULL;
    }
    if (s_amr_nb_decoder != NULL) {
        dashcdg_amr_nb_decoder_destroy(s_amr_nb_decoder);
        s_amr_nb_decoder = NULL;
    }
}

static int badge_rx_ensure_audio_jitter(void)
{
    if (s_audio_jb != NULL && dashcdg_audio_jitter_capacity(s_audio_jb) > 0U) {
        return 0;
    }
    if (s_audio_jb != NULL) {
        /* Struct exists but init/resize OOM left slot_capacity 0; recover. */
        badge_rx_free_audio_jitter();
    }
    s_audio_jb = (struct dashcdg_audio_jitter_buffer *)heap_caps_calloc(
            1, sizeof(struct dashcdg_audio_jitter_buffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_audio_jb == NULL) {
        ESP_LOGW(TAG, "audio jitter: control struct OOM");
        return -1;
    }
    dashcdg_audio_jitter_init(s_audio_jb);
#if defined(DASHCDG_AUDIO_JITTER_HEAP_BACKED) && DASHCDG_AUDIO_JITTER_HEAP_BACKED
    if (dashcdg_audio_jitter_capacity(s_audio_jb) == 0U) {
        if (dashcdg_audio_jitter_resize(s_audio_jb, (size_t)BADGE_RX_AUDIO_JB_SLOTS_FALLBACK_A)) {
            ESP_LOGI(TAG, "audio jitter: using %u slots (default pool OOM under video/anchor load)",
                     (unsigned)dashcdg_audio_jitter_capacity(s_audio_jb));
        } else if (dashcdg_audio_jitter_resize(s_audio_jb, (size_t)BADGE_RX_AUDIO_JB_SLOTS_FALLBACK_B)) {
            ESP_LOGW(TAG, "audio jitter: using %u slots (tight heap; A/V + anchors)",
                     (unsigned)dashcdg_audio_jitter_capacity(s_audio_jb));
        } else {
            ESP_LOGW(TAG, "audio jitter: slot/payload pool alloc failed — no audio until more heap");
            badge_rx_free_audio_jitter();
            return -1;
        }
    }
#endif
    return 0;
}

/** Stereo interleaved → mono, in-place (output length `frame_count`; needs 2 * frame_count samples in `pcm`). */
static void badge_rx_opus_stereo_interleaved_to_mono_inplace(int16_t *pcm, size_t frame_count)
{
    size_t k;

    if (pcm == NULL || frame_count == 0U) {
        return;
    }
    for (k = frame_count; k > 0U; --k) {
        size_t i = k - 1U;
        int32_t s = (int32_t)pcm[i * 2U] + (int32_t)pcm[i * 2U + 1U];

        pcm[i] = (int16_t)(s / 2);
    }
}

static int badge_rx_ensure_opus_decoder(uint8_t frame_ms, const uint8_t *opus_pkt, size_t opus_len)
{
    uint16_t sr = s_announced_audio_sample_rate != 0U ? s_announced_audio_sample_rate : 48000U;
    int probed_ch = -1;
    uint8_t ch;
    uint8_t dec_frame_ms;

    if (opus_pkt != NULL && opus_len != 0U) {
        probed_ch = dashcdg_opus_probe_packet_channels(opus_pkt, opus_len);
        if (probed_ch == 1 || probed_ch == 2) {
            s_opus_pkt_channels = (uint8_t)probed_ch;
        }
    }
    if (probed_ch == 1 || probed_ch == 2) {
        ch = (uint8_t)probed_ch;
    } else if (s_opus_pkt_channels != 0U) {
        ch = s_opus_pkt_channels;
    } else {
        ch = s_announced_audio_channels != 0U ? s_announced_audio_channels : 1U;
    }

#if CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY
    /* Mono decoder + smaller heap blob; stereo Opus packets will not decode (send mono from TX). */
    ch = 1U;
#endif

    dec_frame_ms = frame_ms;
    if (dec_frame_ms == 0U) {
        dec_frame_ms = s_announced_audio_frame_ms != 0U ? s_announced_audio_frame_ms : 20U;
    }
    if (opus_pkt != NULL && opus_len != 0U) {
        int spc = dashcdg_opus_probe_packet_samples_per_channel(opus_pkt, opus_len, (int)sr);

        if (spc > 0) {
            uint64_t num = (uint64_t)spc * 1000ULL + (uint64_t)sr - 1ULL;
            uint32_t need_ms = (uint32_t)(num / (uint64_t)sr);

            if (need_ms > 60U) {
                need_ms = 60U;
            }
            if (need_ms > dec_frame_ms) {
                dec_frame_ms = (uint8_t)need_ms;
            }
        }
    } else if (s_opus_decoder_ready != 0U && s_opus_decoder_frame_ms != 0U) {
        /* SKIP/PLC: no TOC — keep last decoder frame duration so packet-loss decode matches stream. */
        dec_frame_ms = s_opus_decoder_frame_ms;
    }

    if (s_opus_decoder_ready != 0U && sr == s_opus_decoder_sr && ch == s_opus_decoder_ch &&
            dec_frame_ms == s_opus_decoder_frame_ms) {
        return 0;
    }
    badge_rx_opus_decoder_reset();
    badge_rx_drop_amr_decoders();
    {
        int opus_err = OPUS_OK;

        if (!dashcdg_opus_decoder_init(&s_opus_decoder, (int)sr, (int)ch, (int)dec_frame_ms, &opus_err)) {
            static uint64_t s_last_opus_init_warn_ms;

            const uint64_t now_ms = dashcdg_clock_now_ms();
            if (s_last_opus_init_warn_ms == 0U || now_ms - s_last_opus_init_warn_ms >= 3000U) {
                s_last_opus_init_warn_ms = now_ms;
                ESP_LOGW(
                        TAG,
                        "opus decoder init failed sr=%u ch=%u frame_ms=%u opus_err=%d (%s) "
                        "internal_free=%u internal_largest=%u",
                        (unsigned)sr,
                        (unsigned)ch,
                        (unsigned)dec_frame_ms,
                        opus_err,
                        opus_strerror(opus_err),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                );
            }
            return -1;
        }
    }
    s_opus_decoder_sr = sr;
    s_opus_decoder_ch = ch;
    s_opus_decoder_frame_ms = dec_frame_ms;
    s_opus_decoder_ready = 1;
    (void)badge_rx_ensure_pcm_scratch();
    return 0;
}

static void badge_rx_free_audio_jitter(void)
{
    s_pump_defer_audio_drain = 0U;
    if (s_audio_jb != NULL) {
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
        dashcdg_audio_jitter_release(s_audio_jb);
#else
        dashcdg_audio_jitter_clear(s_audio_jb);
#endif
        heap_caps_free(s_audio_jb);
        s_audio_jb = NULL;
    }
    if (s_amr_pcm48_scratch != NULL) {
        heap_caps_free(s_amr_pcm48_scratch);
        s_amr_pcm48_scratch = NULL;
        s_pcm_scratch_samples = 0U;
    }
}

static void badge_rx_resize_audio_locked(size_t slots)
{
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    if (s_audio_jb != NULL) {
        size_t before_occ = dashcdg_audio_jitter_occupied_count(s_audio_jb);
        if (!dashcdg_audio_jitter_resize(s_audio_jb, slots)) {
            s_memory_profile_resize_failures++;
            return;
        }
        {
            size_t after_occ = dashcdg_audio_jitter_occupied_count(s_audio_jb);
            if (before_occ > after_occ) {
                s_memory_profile_resize_audio_dropped += (uint32_t)(before_occ - after_occ);
            }
        }
    }
#else
    (void)slots;
#endif
}

static void badge_rx_resize_video_locked(size_t slots)
{
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    if (s_jb != NULL) {
        size_t before_occ = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
        if (!dashcdg_cdg_batch_jitter_resize(s_jb, slots)) {
            s_memory_profile_resize_failures++;
            return;
        }
        {
            size_t after_occ = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
            if (before_occ > after_occ) {
                s_memory_profile_resize_video_dropped += (uint32_t)(before_occ - after_occ);
            }
        }
    }
#else
    (void)slots;
#endif
}

static void badge_rx_apply_memory_profile_locked(void)
{
    badge_rx_memory_profile_t new_profile;

    if (s_audio_decode_enabled && s_video_decode_enabled) {
        new_profile = BADGE_RX_MEM_PROFILE_BALANCED;
    } else if (s_audio_decode_enabled) {
        new_profile = BADGE_RX_MEM_PROFILE_AUDIO_PRIORITY;
    } else if (s_video_decode_enabled) {
        new_profile = BADGE_RX_MEM_PROFILE_VIDEO_PRIORITY;
    } else {
        new_profile = BADGE_RX_MEM_PROFILE_MINIMAL;
    }
    if (new_profile != s_memory_profile) {
        s_memory_profile = new_profile;
        s_memory_profile_switches++;
    }

    if (!s_audio_decode_enabled) {
        s_cdg_jb_headroom_slots = BADGE_RX_JB_HEADROOM_VIDEO_PRIORITY;
        /*
         * Audio off: release jitter + decode scratch RAM so the remaining video path keeps
         * more internal heap headroom.
         */
        badge_rx_free_audio_jitter();
    } else {
        s_cdg_jb_headroom_slots = BADGE_RX_JB_HEADROOM_EFFECTIVE;
        (void)badge_rx_ensure_audio_jitter();
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
        if (s_audio_jb != NULL) {
            size_t audio_slots =
                    (s_memory_profile == BADGE_RX_MEM_PROFILE_AUDIO_PRIORITY) ? BADGE_RX_AUDIO_SLOTS_PRIORITY
                                                                              : BADGE_RX_AUDIO_SLOTS_BALANCED;
            badge_rx_resize_audio_locked(audio_slots);
        }
#endif
    }

    if (!s_video_decode_enabled) {
        /*
         * Video off: release CDG working set. With heap mode this returns CDG/jitter RAM to heap;
         * with static mode, pointers are detached and can be rebound on re-enable.
         */
        badge_rx_free_heap();
        s_jitter_cdg_primed = 0;
        s_last_cdg_apply_local_ms = 0U;
        s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    } else {
        int need_init = (s_cdg == NULL || s_jb == NULL) ? 1 : 0;
        if (need_init && badge_rx_ensure_heap() == 0) {
            dashcdg_cdg_state_init(s_cdg);
            dashcdg_cdg_batch_jitter_init(s_jb);
            dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
            s_jitter_cdg_primed = 0;
            s_last_cdg_apply_local_ms = 0U;
            s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
        }
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
        if (s_jb != NULL) {
            size_t video_slots = s_audio_decode_enabled ? BADGE_RX_VIDEO_SLOTS_BALANCED : BADGE_RX_VIDEO_SLOTS_PRIORITY;
            badge_rx_resize_video_locked(video_slots);
        }
#endif
        (void)badge_rx_ensure_blit_scratch();
    }

#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    if (!s_audio_decode_enabled && !s_video_decode_enabled && s_audio_jb != NULL) {
        badge_rx_resize_audio_locked(BADGE_RX_AUDIO_SLOTS_MINIMAL);
    }
#endif
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    if (!s_audio_decode_enabled && !s_video_decode_enabled && s_jb != NULL) {
        badge_rx_resize_video_locked(BADGE_RX_VIDEO_SLOTS_MINIMAL);
    }
#endif
}

/**
 * Adaptive jitter resize: caller must hold `s_mtx`. Heap snapshot is taken **before** this call
 * (outside the long drain+mutex section) so `heap_caps_*` does not extend LVGL-critical mutex holds (T7).
 */
static void badge_rx_adapt_jitter_capacity_locked_with_heap(uint64_t now_ms, uint32_t free_internal,
                                                            uint32_t largest_internal)
{
    uint32_t min_internal_free;
    uint32_t dynamic_budget_bytes;
    uint32_t dynamic_budget_contig;
    uint8_t pressure_fast = 0U;

    if (now_ms < s_last_adapt_tune_ms || (now_ms - s_last_adapt_tune_ms) < (uint64_t)BADGE_RX_ADAPT_TUNE_MS) {
        return;
    }
    s_last_adapt_tune_ms = now_ms;
    if (!s_audio_decode_enabled && !s_video_decode_enabled) {
        return;
    }
    min_internal_free = (s_audio_decode_enabled && s_video_decode_enabled)
                            ? (uint32_t)BADGE_RX_ADAPT_MIN_INTERNAL_FREE_BOTH_ON
                            : (uint32_t)BADGE_RX_ADAPT_MIN_INTERNAL_FREE_SINGLE_ON;
    if (free_internal < min_internal_free || largest_internal < (uint32_t)BADGE_RX_ADAPT_MIN_LARGEST_BLOCK) {
        return;
    }
    dynamic_budget_bytes = free_internal - min_internal_free;
    dynamic_budget_contig = largest_internal - (uint32_t)BADGE_RX_ADAPT_MIN_LARGEST_BLOCK;
    if (dynamic_budget_contig < dynamic_budget_bytes) {
        dynamic_budget_bytes = dynamic_budget_contig;
    }
    s_last_adapt_budget_contig_bytes = dynamic_budget_contig;
    s_last_adapt_budget_bytes = dynamic_budget_bytes;
    if (s_last_adapt_resize_fail_ms != 0U &&
        (now_ms - s_last_adapt_resize_fail_ms) < (uint64_t)BADGE_RX_ADAPT_FAIL_COOLDOWN_MS) {
        /* Recent resize OOM/fragmentation: brief cooldown prevents fail counter runaway. */
        return;
    }
    if (s_stats.jb_evict_rounds > s_last_adapt_jb_evict_rounds) {
        pressure_fast = 1U;
    }
    s_last_adapt_jb_evict_rounds = s_stats.jb_evict_rounds;
    s_last_adapt_fast_mode = pressure_fast;
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    if (s_audio_decode_enabled && s_audio_jb != NULL) {
        size_t cap = dashcdg_audio_jitter_capacity(s_audio_jb);
        size_t occ = dashcdg_audio_jitter_occupied_count(s_audio_jb);
        size_t slot_bytes = sizeof(struct dashcdg_audio_jitter_frame) + (size_t)DASHCDG_AUDIO_JITTER_MAX_PAYLOAD;
        uint32_t budget_audio = s_video_decode_enabled ? (dynamic_budget_bytes / 3U) : dynamic_budget_bytes;
        s_last_adapt_budget_audio_bytes = budget_audio;
        /*
         * Budget-derived ceiling for how large the adaptive ring may grow. Must cap *down* only:
         * the old `if (max_slots < BADGE_RX_AUDIO_SLOTS_MAX) max_slots = MAX` was a floor — it forced
         * a minimum target of 80 slots whenever heap budget was tight, driving dozens of grows +
         * resize OOM (calloc peak = old+new buffers) and runaway rsz fail on ESP32.
         */
        size_t max_slots = cap + (slot_bytes > 0U ? ((size_t)budget_audio / slot_bytes) : 0U);
        if (max_slots > (size_t)BADGE_RX_ADAPT_AUDIO_HARD_MAX_SLOTS) {
            max_slots = (size_t)BADGE_RX_ADAPT_AUDIO_HARD_MAX_SLOTS;
        }
        if (max_slots > (size_t)BADGE_RX_AUDIO_SLOTS_MAX) {
            max_slots = (size_t)BADGE_RX_AUDIO_SLOTS_MAX;
        }
        size_t step = pressure_fast ? (size_t)BADGE_RX_ADAPT_STEP_FAST_SLOTS : (size_t)BADGE_RX_ADAPT_STEP_SLOTS;
        size_t occ_pct = pressure_fast ? (size_t)BADGE_RX_ADAPT_OCC_PCT_FAST : (size_t)BADGE_RX_ADAPT_OCC_PCT;
        if (cap > 0U && (occ * 100U) >= ((size_t)BADGE_RX_ADAPT_BURST_OCC_PCT * cap)) {
            s_last_adapt_burst_ms = now_ms;
        }
        if (s_adapt_resize_fail_streak >= (uint8_t)BADGE_RX_ADAPT_FAIL_STREAK_STEP_DOWN && step > (size_t)BADGE_RX_ADAPT_STEP_SLOTS) {
            step = (size_t)BADGE_RX_ADAPT_STEP_SLOTS;
        }
        if (cap > 0U && cap < max_slots &&
            (occ * 100U) >= (occ_pct * cap)) {
            size_t grow = cap + step;
            uint32_t fail_before = s_memory_profile_resize_failures;
            if (grow > max_slots) {
                grow = max_slots;
            }
            if (grow > cap) {
                badge_rx_resize_audio_locked(grow);
                if (s_memory_profile_resize_failures > fail_before) {
                    s_last_adapt_resize_fail_ms = now_ms;
                    if (s_adapt_resize_fail_streak < 255U) {
                        s_adapt_resize_fail_streak++;
                    }
                } else {
                    s_memory_profile_adaptive_grows++;
                    s_last_adapt_grow_ms = now_ms;
                    s_adapt_resize_fail_streak = 0U;
                }
            }
        }
        s_last_adapt_audio_max_slots = (uint16_t)max_slots;
        /*
         * Audio-only karaoke: shrinking the ring after a burst (heap looked "healthy") recovers RAM
         * but leaves too little reorder headroom — Opus/AMR all hit insert-evict + SKIP the same way.
         * Keep the grown ring until video decode is on (balanced A/V shrink still applies).
         */
        if (s_video_decode_enabled != 0U && (now_ms - s_last_adapt_grow_ms) >= (uint64_t)BADGE_RX_ADAPT_SHRINK_HOLDOFF_MS &&
            (now_ms - s_last_adapt_burst_ms) >= (uint64_t)BADGE_RX_ADAPT_BURST_HOLD_MS &&
            cap > (size_t)BADGE_RX_AUDIO_SLOTS_BALANCED &&
            (occ * 100U) <= ((size_t)BADGE_RX_ADAPT_SHRINK_OCC_PCT * cap)) {
            size_t target = (size_t)BADGE_RX_AUDIO_SLOTS_BALANCED;
            if (target < cap) {
                badge_rx_resize_audio_locked(target);
                s_memory_profile_adaptive_shrinks++;
            }
        }
    }
#else
    s_last_adapt_audio_max_slots = (uint16_t)BADGE_RX_AUDIO_SLOTS_MAX;
    s_last_adapt_budget_audio_bytes = 0U;
#endif
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    if (s_video_decode_enabled && s_jb != NULL) {
        size_t cap = dashcdg_cdg_batch_jitter_capacity(s_jb);
        size_t occ = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
        size_t slot_bytes = sizeof(struct dashcdg_cdg_batch_jitter_frame) +
                            ((size_t)DASHCDG_MAX_CDG_BATCH_PACKETS * (size_t)DASHCDG_SUBCHANNEL_PACKET_BYTES);
        uint32_t budget_video = s_audio_decode_enabled ? ((dynamic_budget_bytes * 2U) / 3U) : dynamic_budget_bytes;
        s_last_adapt_budget_video_bytes = budget_video;
        size_t max_slots = cap + (slot_bytes > 0U ? ((size_t)budget_video / slot_bytes) : 0U);
        if (max_slots > (size_t)BADGE_RX_ADAPT_VIDEO_HARD_MAX_SLOTS) {
            max_slots = (size_t)BADGE_RX_ADAPT_VIDEO_HARD_MAX_SLOTS;
        }
        if (max_slots > (size_t)BADGE_RX_VIDEO_SLOTS_MAX) {
            max_slots = (size_t)BADGE_RX_VIDEO_SLOTS_MAX;
        }
        size_t step = pressure_fast ? (size_t)BADGE_RX_ADAPT_STEP_FAST_SLOTS : (size_t)BADGE_RX_ADAPT_STEP_SLOTS;
        size_t occ_pct = pressure_fast ? (size_t)BADGE_RX_ADAPT_OCC_PCT_FAST : (size_t)BADGE_RX_ADAPT_OCC_PCT;
        if (cap > 0U && (occ * 100U) >= ((size_t)BADGE_RX_ADAPT_BURST_OCC_PCT * cap)) {
            s_last_adapt_burst_ms = now_ms;
        }
        if (s_adapt_resize_fail_streak >= (uint8_t)BADGE_RX_ADAPT_FAIL_STREAK_STEP_DOWN && step > (size_t)BADGE_RX_ADAPT_STEP_SLOTS) {
            step = (size_t)BADGE_RX_ADAPT_STEP_SLOTS;
        }
        if (cap > 0U && cap < max_slots &&
            (occ * 100U) >= (occ_pct * cap)) {
            size_t grow = cap + step;
            uint32_t fail_before = s_memory_profile_resize_failures;
            if (grow > max_slots) {
                grow = max_slots;
            }
            if (grow > cap) {
                badge_rx_resize_video_locked(grow);
                if (s_memory_profile_resize_failures > fail_before) {
                    s_last_adapt_resize_fail_ms = now_ms;
                    if (s_adapt_resize_fail_streak < 255U) {
                        s_adapt_resize_fail_streak++;
                    }
                } else {
                    s_memory_profile_adaptive_grows++;
                    s_last_adapt_grow_ms = now_ms;
                    s_adapt_resize_fail_streak = 0U;
                }
            }
        }
        s_last_adapt_video_max_slots = (uint16_t)max_slots;
        if ((now_ms - s_last_adapt_grow_ms) >= (uint64_t)BADGE_RX_ADAPT_SHRINK_HOLDOFF_MS &&
            (now_ms - s_last_adapt_burst_ms) >= (uint64_t)BADGE_RX_ADAPT_BURST_HOLD_MS &&
            cap > (s_audio_decode_enabled ? (size_t)BADGE_RX_VIDEO_SLOTS_BALANCED : (size_t)BADGE_RX_VIDEO_SLOTS_PRIORITY) &&
            (occ * 100U) <= ((size_t)BADGE_RX_ADAPT_SHRINK_OCC_PCT * cap)) {
            size_t target = s_audio_decode_enabled ? (size_t)BADGE_RX_VIDEO_SLOTS_BALANCED : (size_t)BADGE_RX_VIDEO_SLOTS_PRIORITY;
            if (target < cap) {
                badge_rx_resize_video_locked(target);
                s_memory_profile_adaptive_shrinks++;
            }
        }
    }
#else
    s_last_adapt_video_max_slots = (uint16_t)BADGE_RX_VIDEO_SLOTS_MAX;
    s_last_adapt_budget_video_bytes = 0U;
#endif
}

static void badge_rx_adapt_jitter_capacity_unblocked(uint64_t now_ms)
{
    uint32_t fi = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t la = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(12)) != pdTRUE) {
        return;
    }
    badge_rx_adapt_jitter_capacity_locked_with_heap(now_ms, fi, la);
    xSemaphoreGive(s_mtx);
}

static void badge_rx_audio_last_mono_reset(void)
{
    s_rx_last_mono48_valid = 0U;
    s_rx_last_mono48_len = 0U;
    s_audio_degrade_loss_run = 0U;
}

static void badge_rx_audio_last_mono_store(const int16_t *pcm, size_t n)
{
    if (pcm == NULL || n == 0U) {
        return;
    }
    if (n > (size_t)BADGE_RX_AUDIO_LAST_MONO_CAP) {
        n = (size_t)BADGE_RX_AUDIO_LAST_MONO_CAP;
    }
    memcpy(s_rx_last_mono48, pcm, n * sizeof(int16_t));
    s_rx_last_mono48_len = (uint16_t)n;
    s_rx_last_mono48_valid = 1U;
}

static int32_t badge_rx_audio_noise_q15(void)
{
    uint32_t x;

    if (s_audio_degrade_lfsr == 0U) {
        s_audio_degrade_lfsr = esp_random() | 1U;
    }
    x = s_audio_degrade_lfsr;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_audio_degrade_lfsr = x ? x : 1U;
    return (int32_t)(x & 0x7FFF) - 16384;
}

static void badge_rx_audio_fill_degraded(int16_t *dst, size_t n)
{
    size_t cap = s_rx_last_mono48_len;
    size_t i;

    if (cap > (size_t)BADGE_RX_AUDIO_LAST_MONO_CAP) {
        cap = (size_t)BADGE_RX_AUDIO_LAST_MONO_CAP;
    }
    if (cap == 0U || s_rx_last_mono48_valid == 0U) {
        for (i = 0U; i < n; i++) {
            dst[i] = 0;
        }
        return;
    }
    for (i = 0U; i < n; i++) {
        int32_t l = (int32_t)s_rx_last_mono48[i % cap];
        int32_t nz = badge_rx_audio_noise_q15();
        int32_t mix = (l * (int32_t)BADGE_RX_AUDIO_DEGRADE_LAST_WEIGHT +
                nz * (int32_t)BADGE_RX_AUDIO_DEGRADE_NOISE_WEIGHT) /
                100;
        {
            uint32_t att = 256U;

            if (s_audio_degrade_loss_run > 8U) {
                att = 210U;
            }
            if (s_audio_degrade_loss_run > 22U) {
                att = 168U;
            }
            if (s_audio_degrade_loss_run > 45U) {
                att = 130U;
            }
            mix = (mix * (int32_t)att) / 256;
        }

        if (mix > 32767) {
            mix = 32767;
        } else if (mix < -32768) {
            mix = -32768;
        }
        dst[i] = (int16_t)mix;
    }
}

static void badge_rx_audio_no_dac_watch_note_rx(uint64_t local_now_ms)
{
    if (s_audio_rx_since_dac_push == 0U) {
        s_audio_rx_since_dac_push_start_ms = local_now_ms;
    }
    s_audio_rx_since_dac_push++;
    if (s_audio_rx_since_dac_push_start_ms == 0U || local_now_ms <= s_audio_rx_since_dac_push_start_ms) {
        return;
    }
    if (local_now_ms - s_audio_rx_since_dac_push_start_ms < BADGE_RX_AUDIO_RX_NO_DAC_WARN_MS) {
        return;
    }
    if (s_audio_rx_no_dac_last_warn_ms != 0U &&
            local_now_ms - s_audio_rx_no_dac_last_warn_ms < BADGE_RX_AUDIO_RX_NO_DAC_WARN_REPEAT_MS) {
        return;
    }
    s_audio_rx_no_dac_last_warn_ms = local_now_ms;
    s_audio_rx_no_dac_warn_count++;
    ESP_LOGW(
            TAG,
            "audio packets arriving but no DAC push for >%u ms (rx_since_push=%u jb_occ=%u decode=%u)",
            (unsigned)BADGE_RX_AUDIO_RX_NO_DAC_WARN_MS,
            (unsigned)s_audio_rx_since_dac_push,
            (unsigned)((s_audio_jb != NULL) ? dashcdg_audio_jitter_occupied_count(s_audio_jb) : 0U),
            (unsigned)s_audio_decode_enabled);
}

static void badge_rx_audio_no_dac_watch_note_push(void)
{
    s_audio_rx_since_dac_push = 0U;
    s_audio_rx_since_dac_push_start_ms = 0U;
}

static void badge_rx_audio_start_gate_stall_clear(void)
{
    s_audio_start_gate_stall_begin_ms = 0U;
}

#if defined(ESP_PLATFORM)
static void badge_rx_audio_start_gate_stall_note_block(
        uint64_t local_now_ms,
        uint32_t occ,
        uint32_t buffered_ms,
        uint32_t required_ms,
        const char *reason
)
{
    if (occ == 0U) {
        s_audio_start_gate_stall_begin_ms = 0U;
        return;
    }
    if (s_audio_start_gate_stall_begin_ms == 0U) {
        s_audio_start_gate_stall_begin_ms = local_now_ms;
    }
    if (local_now_ms - s_audio_start_gate_stall_begin_ms <= 500U) {
        return;
    }
    if (s_audio_start_gate_stall_last_log_ms != 0U &&
            local_now_ms - s_audio_start_gate_stall_last_log_ms < 4000U) {
        return;
    }
    s_audio_start_gate_stall_last_log_ms = local_now_ms;
    ESP_LOGW(
            TAG,
            "audio startup gate: %s (>500 ms, jb_occ=%u buf=%ums need=%ums have_clock=%d primed=%d)",
            reason,
            (unsigned)occ,
            (unsigned)buffered_ms,
            (unsigned)required_ms,
            (int)s_stats.have_clock,
            (int)s_audio_decode_primed);
}
#else
static void badge_rx_audio_start_gate_stall_note_block(
        uint64_t local_now_ms,
        uint32_t occ,
        uint32_t buffered_ms,
        uint32_t required_ms,
        const char *reason
)
{
    (void)local_now_ms;
    (void)occ;
    (void)buffered_ms;
    (void)required_ms;
    (void)reason;
}
#endif

static int badge_rx_audio_start_gate_holding(uint64_t local_now_ms)
{
    uint32_t buffered_ms = 0U;
    uint32_t required_ms;
    uint32_t frame_ms;
    uint32_t occ = 0U;

    /*
     * Startup gate is strictly pre-start. Once we've presented audio at least once,
     * never re-arm this gate or playback will "sawtooth" into repeated hold/release
     * and sound permanently choppy.
     */
    if (s_last_presented_audio_timestamp_ms != 0U || s_audio_decode_primed != 0) {
        badge_rx_audio_start_gate_stall_clear();
        return 0;
    }

    if (s_audio_jb != NULL) {
        occ = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
        frame_ms = (uint32_t)(s_announced_audio_frame_ms != 0U ? s_announced_audio_frame_ms : 20U);
        buffered_ms = occ * frame_ms;
    }
    required_ms = (uint32_t)(s_announced_playout_delay_ms != 0U ? (s_announced_playout_delay_ms / 2U) : 170U);
    frame_ms = (uint32_t)(s_announced_audio_frame_ms != 0U ? s_announced_audio_frame_ms : 20U);
    {
        uint32_t frame_window_ms = frame_ms * 6U;
        if (frame_window_ms < 120U) {
            frame_window_ms = 120U;
        }
        if (frame_window_ms > BADGE_RX_AUDIO_START_BUFFER_CAP_MS) {
            frame_window_ms = BADGE_RX_AUDIO_START_BUFFER_CAP_MS;
        }
        if (required_ms > frame_window_ms) {
            required_ms = frame_window_ms;
        }
    }
    if (buffered_ms < required_ms) {
        badge_rx_audio_start_gate_stall_note_block(local_now_ms, occ, buffered_ms, required_ms, "buffer_lt_required");
        return 1;
    }
#if defined(ESP_PLATFORM)
    /*
     * Deep backlog: start playback immediately — do not wait MIN_HOLD/sync-leader gates.
     * Otherwise “RX packets, jb full, no PCM” when clock packets are delayed vs media bursts.
     */
    if (buffered_ms >= required_ms + (uint32_t)frame_ms * 10U) {
        badge_rx_audio_start_gate_stall_clear();
        return 0;
    }
#endif
    if (s_active_session_local_start_ms == 0U || local_now_ms <= s_active_session_local_start_ms) {
        badge_rx_audio_start_gate_stall_clear();
        return 0;
    }
    {
        uint64_t since_ms = local_now_ms - s_active_session_local_start_ms;
        if (since_ms < BADGE_RX_AUDIO_START_MIN_HOLD_MS) {
            badge_rx_audio_start_gate_stall_note_block(local_now_ms, occ, buffered_ms, required_ms, "min_hold_wall");
            return 1;
        }
        if (since_ms >= BADGE_RX_AUDIO_START_MAX_HOLD_MS) {
            badge_rx_audio_start_gate_stall_clear();
            return 0;
        }
    }
    if (s_stats.have_clock && s_active_session_start_ms != 0U && s_sync_local_ms != 0U) {
        uint64_t sender_now_ms;
        uint32_t target_ms;

        sender_now_ms = badge_rx_estimated_playback_ms_at_local(local_now_ms);
        target_ms = s_sync_group_target_latency_ms != 0U
                ? (uint32_t)s_sync_group_target_latency_ms
                : (uint32_t)(s_announced_playout_delay_ms != 0U ? s_announced_playout_delay_ms : 500U);
        if (target_ms < 500U) {
            target_ms = 500U;
        }
        if (target_ms > 1000U) {
            target_ms = 1000U;
        }
        if (sender_now_ms > s_active_session_start_ms) {
            uint64_t sender_progress_ms = sender_now_ms - s_active_session_start_ms;
            if (sender_progress_ms + 40U < (uint64_t)target_ms) {
                badge_rx_audio_start_gate_stall_note_block(local_now_ms, occ, buffered_ms, required_ms, "sync_leader_latency");
                return 1;
            }
        }
    }
    badge_rx_audio_start_gate_stall_clear();
    return 0;
}

/**
 * Nominal PCM rate for ESP32 `dac_continuous`.
 * Fixed at 48 kHz so Opus ↔ AMR switches do not reopen DMA at 8/16/48 kHz (heap + `ESP_ERR_NO_MEM`) or
 * play short native-length buffers at the wrong line rate (tinny / dropouts).
 */
static uint32_t badge_rx_karaoke_nominal_pcm_hz(void)
{
    return BADGE_RX_KARAOKE_LINE_HZ;
}

/** Opus decoder Fs (session); used to resample PCM to `BADGE_RX_KARAOKE_LINE_HZ` before DAC. */
static uint32_t badge_rx_opus_pcm_native_hz(void)
{
    if (s_opus_decoder_ready != 0U && s_opus_decoder.sample_rate > 0) {
        return (uint32_t)s_opus_decoder.sample_rate;
    }
    return s_announced_audio_sample_rate != 0U ? (uint32_t)s_announced_audio_sample_rate : BADGE_RX_KARAOKE_LINE_HZ;
}

/** Native Hz for `badge_rx_audio_push_degraded_mono` filler (matches decode path timeline). */
static uint32_t badge_rx_degraded_pcm_native_hz(void)
{
    switch (badge_rx_effective_audio_codec_id()) {
    case DASHCDG_V4_AUDIO_CODEC_OPUS:
        return badge_rx_opus_pcm_native_hz();
    case DASHCDG_V4_AUDIO_CODEC_AMR_WB:
        return 16000U;
    case DASHCDG_V4_AUDIO_CODEC_AMR_NB:
        return 8000U;
    default:
        return BADGE_RX_KARAOKE_LINE_HZ;
    }
}

static size_t badge_rx_degraded_mono_samples(void)
{
    uint8_t c = badge_rx_effective_audio_codec_id();
    uint32_t fms;

    if (c == DASHCDG_V4_AUDIO_CODEC_OPUS && s_opus_decoder_ready) {
        return (size_t)s_opus_decoder.frame_size;
    }
    fms = s_announced_audio_frame_ms != 0U ? (uint32_t)s_announced_audio_frame_ms : 20U;
    if (c == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        return (size_t)(((uint32_t)16000U * fms) / 1000U);
    }
    if (c == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        return (size_t)(((uint32_t)8000U * fms) / 1000U);
    }
    return (size_t)(((uint32_t)BADGE_RX_KARAOKE_LINE_HZ * fms) / 1000U);
}

static int badge_rx_audio_push_mono_gated(uint64_t local_now_ms, const int16_t *mono, size_t mono_samples)
{
    uint32_t nom_hz;

    if (mono == NULL || mono_samples == 0U) {
        return 0;
    }
    if (badge_rx_audio_start_gate_holding(local_now_ms)) {
        return 0;
    }
    nom_hz = badge_rx_karaoke_nominal_pcm_hz();
    if (!dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(nom_hz)) {
        /*
         * Defensive recovery: when audio decode is toggled on mid-session or amp state is stale,
         * explicitly arm amp then retry DAC begin once before declaring host-underrun.
         */
        dashcdg_platform_hw_karaoke_amp_arm_for_rx();
        if (!dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(nom_hz)) {
            s_stats.v4_audio_dac_begin_fail++;
            return 0;
        }
    }
    dashcdg_platform_hw_karaoke_dac_push_mono_s16(mono, mono_samples);
    badge_rx_audio_no_dac_watch_note_push();
    return 1;
}

static void badge_rx_audio_push_degraded_mono(uint64_t local_now_ms, size_t mono_samples)
{
    int16_t buf[960];

    if (mono_samples == 0U || mono_samples > 960U) {
        mono_samples = 960U;
    }
    if (s_audio_degrade_loss_run < 65535U) {
        s_audio_degrade_loss_run++;
    }
    s_stats.v4_audio_degraded_push++;
    badge_rx_audio_fill_degraded(buf, mono_samples);
    (void)badge_rx_push_mono_resampled_to_karaoke_line(local_now_ms, buf, mono_samples, badge_rx_degraded_pcm_native_hz());
}

/**
 * Decode-stage PCM at `native_hz` → line-rate mono for `dac_continuous` @ BADGE_RX_KARAOKE_LINE_HZ.
 * Returns 1 if samples reached the DAC path (may still be gated — same as `push_mono_gated` truth).
 */
static int badge_rx_push_mono_resampled_to_karaoke_line(
        uint64_t local_now_ms,
        const int16_t *pcm_native,
        size_t n_native,
        uint32_t native_hz
)
{
    size_t out_n;

    if (pcm_native == NULL || n_native == 0U) {
        return 0;
    }
    if (native_hz == BADGE_RX_KARAOKE_LINE_HZ) {
        if (!badge_rx_audio_push_mono_gated(local_now_ms, pcm_native, n_native)) {
            return 0;
        }
        badge_rx_audio_last_mono_store(pcm_native, n_native);
        return 1;
    }
    if (native_hz == 0U) {
        return 0;
    }
    /* Integer ceil so SRC output length never truncates vs true ratio (avoids slow / grainy drift). */
    out_n = (size_t)((n_native * (uint64_t)BADGE_RX_KARAOKE_LINE_HZ + (uint64_t)native_hz - 1ULL) / (uint64_t)native_hz);
    if (out_n == 0U || out_n > BADGE_RX_RESAMPLE_TO48_CAP) {
        ESP_LOGW(TAG, "resample: out_n=%u native_hz=%lu exceeds staging cap %u", (unsigned)out_n,
                (unsigned long)native_hz, (unsigned)BADGE_RX_RESAMPLE_TO48_CAP);
        return 0;
    }
    dashcdg_pcm_mono_resample_cubic(
            pcm_native,
            n_native,
            native_hz,
            s_badge_rx_resample_to48_staging,
            out_n,
            BADGE_RX_KARAOKE_LINE_HZ);
    if (!badge_rx_audio_push_mono_gated(local_now_ms, s_badge_rx_resample_to48_staging, out_n)) {
        return 0;
    }
    badge_rx_audio_last_mono_store(s_badge_rx_resample_to48_staging, out_n);
    return 1;
}

/** Last wire chunk codec vs session announce — SKIP/APPLY should follow reality once chunks arrived. */
static uint8_t badge_rx_effective_audio_codec_id(void)
{
    if (s_last_audio_codec_inited != 0U) {
        return s_last_audio_codec_id;
    }
    return s_announced_audio_codec_id;
}

/** One-frame PCM scratch for a v4 codec id (0 = unknown / not yet announced). */
static size_t badge_rx_pcm_scratch_samples_need_for_codec_id(uint8_t codec)
{
    if (codec == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        return 320U;
    }
    if (codec == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        return 160U;
    }
    if (codec == DASHCDG_V4_AUDIO_CODEC_OPUS) {
        uint32_t sr = s_announced_audio_sample_rate != 0U ? (uint32_t)s_announced_audio_sample_rate : 48000U;
        uint8_t fms = s_announced_audio_frame_ms != 0U ? s_announced_audio_frame_ms : 20U;

        if (fms > 60U) {
            fms = 60U;
        }
        uint8_t ch = 1U;
#if !CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY
        ch = s_announced_audio_channels != 0U ? s_announced_audio_channels : 1U;
        if (ch > 2U) {
            ch = 2U;
        }
#endif
        {
            uint64_t per_ch = ((uint64_t)sr * (uint64_t)fms + 999ULL) / 1000ULL;
            uint64_t total = per_ch * (uint64_t)ch;

            if (total > (uint64_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX) {
                return (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX;
            }
            return (size_t)total;
        }
    }
    if (codec == 0U) {
        return 0U;
    }
    return (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX;
}

/**
 * int16_t samples required for one decoded frame in `s_amr_pcm48_scratch` (AMR fixed; Opus from decoder or session bound).
 * Uses max(announced, effective, active Opus decoder) so a brief session vs wire skew cannot shrink the
 * buffer below what the next jitter APPLY still needs (codec switch / reorder window).
 */
static size_t badge_rx_pcm_scratch_samples_need(void)
{
    uint8_t eff = badge_rx_effective_audio_codec_id();
    size_t need = badge_rx_pcm_scratch_samples_need_for_codec_id(s_announced_audio_codec_id);
    size_t ne2 = badge_rx_pcm_scratch_samples_need_for_codec_id(eff);

    if (ne2 > need) {
        need = ne2;
    }
    if (s_opus_decoder_ready != 0U) {
        uint64_t n = (uint64_t)(unsigned int)s_opus_decoder.frame_size * (uint64_t)(unsigned int)s_opus_decoder.channels;

        if (n > (uint64_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX) {
            n = (uint64_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX;
        }
        if ((size_t)n > need) {
            need = (size_t)n;
        }
    }
    if (need == 0U) {
        need = 960U;
    }
    /*
     * One stereo Opus frame is interleaved in scratch before mono downmix; do not under-allocate
     * when Opus is in play even if v4_session_info briefly disagrees with the last wire codec id.
     */
    if (need < 1920U && (s_announced_audio_codec_id == (uint8_t)DASHCDG_V4_AUDIO_CODEC_OPUS ||
                eff == (uint8_t)DASHCDG_V4_AUDIO_CODEC_OPUS || s_opus_decoder_ready != 0U)) {
        need = 1920U;
    }
    if (need > (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX) {
        need = (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX;
    }
    return need;
}

/** AMR / Opus decode staging; size follows stream — realloc when session or Opus decoder changes. */
static int badge_rx_ensure_pcm_scratch(void)
{
    size_t need = badge_rx_pcm_scratch_samples_need();

    if (need == 0U) {
        need = 160U;
    }
    if (need > (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX) {
        need = (size_t)BADGE_RX_PCM_SCRATCH_SAMPLES_MAX;
    }
    if (s_amr_pcm48_scratch != NULL && need == s_pcm_scratch_samples) {
        return 0;
    }
    if (s_amr_pcm48_scratch != NULL) {
        heap_caps_free(s_amr_pcm48_scratch);
        s_amr_pcm48_scratch = NULL;
        s_pcm_scratch_samples = 0U;
    }
    s_amr_pcm48_scratch = (int16_t *)heap_caps_malloc(need * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_amr_pcm48_scratch == NULL) {
        return -1;
    }
    s_pcm_scratch_samples = need;
    return 0;
}

static void badge_rx_drain_v4_audio_budget(uint64_t local_now_ms, uint32_t max_steps, uint32_t max_plc_frames)
{
    struct dashcdg_audio_jitter_drain_input din;
    int guard = 0;
    uint32_t plc_frames_left = max_plc_frames;

    if (s_audio_jb == NULL || !s_audio_jb->initialized) {
        return;
    }
    badge_rx_uart_touch_jb_peak_for_chop();
    if (max_steps == 0U) {
        return;
    }

    /*
     * `audio_jitter` starvation gate uses `audio_buffered_ms` vs jitter occupancy only. The native
     * DAC may still hold a partial DMA chunk (not in the jitter ring). Add `s_karaoke_dac_fill`
     * (via `karaoke_dac_get_uart_health`) converted to ms at effective Hz so hole/SKIP does not run
     * while PCM is already queued for DMA.
     */
    uint32_t dac_pending_pcm_ms = 0U;
    {
        uint32_t hz_s = 0U;
        uint32_t fill_u8 = 0U;

        dashcdg_platform_hw_karaoke_dac_get_uart_health(NULL, NULL, &hz_s, NULL, &fill_u8);
        if (hz_s == 0U) {
            hz_s = 48000U;
        }
        if (fill_u8 > 0U) {
            dac_pending_pcm_ms = (uint32_t)((fill_u8 * 1000ULL) / (uint64_t)hz_s);
        }
    }

    for (;;) {
        struct dashcdg_audio_jitter_frame *frame = NULL;
        uint64_t miss_delta = 0U;
        enum dashcdg_audio_drain_step step;
        int start_gate_holding;

        if (++guard > (int)max_steps) {
            break;
        }
        start_gate_holding = badge_rx_audio_start_gate_holding(local_now_ms);
        if (start_gate_holding) {
            /*
             * Do not consume/skip jitter frames while startup gate is holding; otherwise we
             * silently drop early timeline audio and keep a persistent A/V lead on video.
             */
            break;
        }

        memset(&din, 0, sizeof(din));
        din.announced_playout_delay_ms = s_announced_playout_delay_ms;
        if (s_stats.have_clock) {
            din.have_sender_playback = 1;
            din.sender_playback_now_ms = badge_rx_estimated_playback_ms_at_local(local_now_ms);
        }
        din.announced_audio_frame_ms = (s_announced_audio_frame_ms != 0U) ? s_announced_audio_frame_ms : 20U;
        din.late_grace_ms = BADGE_RX_AUDIO_LATE_GRACE_MS;
        /*
         * Desktop uses PortAudio ring depth; badge had audio_stream_started = !have_clock until the first
         * clock_sync — jitter drain SKIP/starvation paths stayed closed ("silence with packets").
         * Treat ≥2 occupied frames as “stream started” so reorder/hole logic matches reality.
         */
        din.audio_stream_started =
                (s_stats.have_clock ||
                        (s_audio_jb != NULL && dashcdg_audio_jitter_occupied_count(s_audio_jb) >= 2))
                        ? 1
                        : 0;
        din.audio_device_null = 0;
        /*
         * No PortAudio ring to query — but `audio_jitter_skip_starvation_gate_open` uses this field.
         * Leaving it at 0 made the gate think the playout buffer was always empty after clock sync,
         * so hole/SKIP fired constantly (PLC + real frames → "picket fence"). Approximate depth from
         * occupied jitter slots × frame duration so skips wait until the ring is actually lean.
         *
         * Add partial native DAC queue depth (bytes @ eff Hz → ms) from `karaoke_dac_get_uart_health`:
         * jitter can show occ=0–1 while PCM is still in `karaoke_dac_push` — without this, the gate opens
         * "empty" skips and UART shows exploding `skip` + `jb=0` with clean decoders (not a codec bug).
         */
        {
            uint32_t occ = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
            uint32_t fms = (uint32_t)din.announced_audio_frame_ms;
            uint32_t jb_ms = occ * fms;
            uint64_t sum = (uint64_t)jb_ms + (uint64_t)dac_pending_pcm_ms;

            if (sum > 4000ULL) {
                sum = 4000ULL;
            }
            din.audio_buffered_ms = (uint32_t)sum;
        }
        din.ms_since_prior_audio_apply = 0U;
        if (s_last_audio_jitter_apply_local_ms != 0U) {
            din.ms_since_prior_audio_apply =
                    (local_now_ms > s_last_audio_jitter_apply_local_ms) ? (local_now_ms - s_last_audio_jitter_apply_local_ms) : 0U;
        }
        din.primed_decode = s_audio_decode_primed;
        /*
         * Audio-only with <2 frames buffered: clock-driven empty-hole SKIP advances next_media_sequence
         * without wire data; inserts then look stale (jb empty, skip count explodes, a_out stalls).
         * Treat as no sender playback until depth returns (same idea as audio_jitter cold join ~407).
         */
        if (badge_rx_is_audio_only_decode() && s_audio_jb != NULL &&
                dashcdg_audio_jitter_occupied_count(s_audio_jb) < 2U) {
            din.have_sender_playback = 0;
        }

        step = dashcdg_audio_jitter_drain_step(s_audio_jb, &din, &frame, &miss_delta);
        if (step == DASHCDG_AUDIO_DRAIN_SKIP) {
            s_recovery_silent_stall_count++;
            s_stats.v4_audio_jitter_skip_events++;
            /*
             * Jitter advanced `next_media_sequence` without a wire frame. If we do not step the AMR-WB
             * `D_IF` state, the next good IF2 frame decodes as garbage (CELP memory no longer matches TX).
             */
            if (badge_rx_effective_audio_codec_id() == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
                uint64_t skip_frames = miss_delta > 0U ? miss_delta : 1U;

                if (skip_frames > (uint64_t)max_steps) {
                    skip_frames = (uint64_t)max_steps;
                }
                if (skip_frames > (uint64_t)plc_frames_left) {
                    skip_frames = (uint64_t)plc_frames_left;
                }
                (void)badge_rx_ensure_pcm_scratch();
                if (s_amr_wb_decoder == NULL) {
                    dashcdg_amr_wb_decoder_create(&s_amr_wb_decoder);
                }
                if (s_amr_wb_decoder != NULL && s_amr_pcm48_scratch != NULL) {
                    for (uint64_t si = 0U; si < skip_frames; si++) {
                        int ln = dashcdg_amr_wb_decoder_run_lost_native(s_amr_wb_decoder, s_amr_pcm48_scratch, 320U);

                        if (ln > 0) {
                            (void)badge_rx_push_mono_resampled_to_karaoke_line(
                                    local_now_ms, s_amr_pcm48_scratch, (size_t)ln, 16000U);
                        } else {
                            badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                        }
                        if (plc_frames_left > 0U) {
                            plc_frames_left--;
                        }
                    }
                }
            } else if (badge_rx_effective_audio_codec_id() == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
                uint64_t skip_frames = miss_delta > 0U ? miss_delta : 1U;

                if (skip_frames > (uint64_t)max_steps) {
                    skip_frames = (uint64_t)max_steps;
                }
                if (skip_frames > (uint64_t)plc_frames_left) {
                    skip_frames = (uint64_t)plc_frames_left;
                }
                (void)badge_rx_ensure_pcm_scratch();
                if (s_amr_nb_decoder == NULL) {
                    dashcdg_amr_nb_decoder_create(&s_amr_nb_decoder);
                }
                if (s_amr_nb_decoder != NULL && s_amr_pcm48_scratch != NULL) {
                    for (uint64_t si = 0U; si < skip_frames; si++) {
                        int ln = dashcdg_amr_nb_decoder_run_lost_native(s_amr_nb_decoder, s_amr_pcm48_scratch, 160U);

                        if (ln > 0) {
                            (void)badge_rx_push_mono_resampled_to_karaoke_line(
                                    local_now_ms, s_amr_pcm48_scratch, (size_t)ln, 8000U);
                        } else {
                            badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                        }
                        if (plc_frames_left > 0U) {
                            plc_frames_left--;
                        }
                    }
                }
            } else if (badge_rx_effective_audio_codec_id() == DASHCDG_V4_AUDIO_CODEC_OPUS) {
                uint64_t skip_frames = miss_delta > 0U ? miss_delta : 1U;
                uint8_t fms = din.announced_audio_frame_ms != 0U ? din.announced_audio_frame_ms : 20U;

                if (skip_frames > (uint64_t)max_steps) {
                    skip_frames = (uint64_t)max_steps;
                }
                if (skip_frames > (uint64_t)plc_frames_left) {
                    skip_frames = (uint64_t)plc_frames_left;
                }
                if (badge_rx_ensure_opus_decoder(fms, NULL, 0) == 0 && badge_rx_ensure_pcm_scratch() == 0 &&
                        s_amr_pcm48_scratch != NULL) {
                    for (uint64_t si = 0U; si < skip_frames; si++) {
                        size_t pcm_need = (size_t)s_opus_decoder.frame_size * (size_t)s_opus_decoder.channels;
                        int plc_ret;

                        if (pcm_need > s_pcm_scratch_samples) {
                            badge_rx_audio_push_degraded_mono(local_now_ms, (size_t)s_opus_decoder.frame_size);
                            break;
                        }
                        plc_ret = dashcdg_opus_decode_packet_loss(&s_opus_decoder, s_amr_pcm48_scratch, pcm_need);
                        if (plc_ret <= 0) {
                            badge_rx_audio_push_degraded_mono(local_now_ms, (size_t)s_opus_decoder.frame_size);
                            continue;
                        }
                        if (s_opus_decoder.channels == 2) {
                            badge_rx_opus_stereo_interleaved_to_mono_inplace(s_amr_pcm48_scratch, (size_t)plc_ret);
                            (void)badge_rx_push_mono_resampled_to_karaoke_line(
                                    local_now_ms, s_amr_pcm48_scratch, (size_t)plc_ret, badge_rx_opus_pcm_native_hz());
                            badge_rx_audio_last_mono_store(s_amr_pcm48_scratch, (size_t)plc_ret);
                        } else {
                            (void)badge_rx_push_mono_resampled_to_karaoke_line(
                                    local_now_ms, s_amr_pcm48_scratch, (size_t)plc_ret, badge_rx_opus_pcm_native_hz());
                            badge_rx_audio_last_mono_store(s_amr_pcm48_scratch, (size_t)plc_ret);
                        }
                        if (plc_frames_left > 0U) {
                            plc_frames_left--;
                        }
                    }
                }
            }
            /* Keep s_audio_decode_primed for real APPLY only — primed after PLC SKIP closes jitter gap-ahead
             * recovery when have_sender_playback=1 (audio-only wedged: a_out=0, deg≈rx). */
            s_last_audio_jitter_apply_local_ms = local_now_ms;
            s_last_presented_audio_timestamp_ms += (uint32_t)din.announced_audio_frame_ms;
        } else if (step == DASHCDG_AUDIO_DRAIN_APPLY && frame != NULL) {
            uint8_t frame_ms = frame->frame_ms != 0U ? frame->frame_ms : din.announced_audio_frame_ms;

            s_audio_degrade_loss_run = 0U;

            if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
                int dec_n;

                if (badge_rx_ensure_pcm_scratch() != 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                if (s_amr_wb_decoder == NULL) {
                    dashcdg_amr_wb_decoder_create(&s_amr_wb_decoder);
                }
                if (s_amr_wb_decoder == NULL) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                dec_n = dashcdg_amr_wb_decoder_run_native(
                        s_amr_wb_decoder,
                        frame->encoded_bytes,
                        frame->encoded_length,
                        s_amr_pcm48_scratch,
                        320U
                );
                if (dec_n <= 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_recovery_zero_buffer_count++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                if (!badge_rx_push_mono_resampled_to_karaoke_line(
                            local_now_ms, s_amr_pcm48_scratch, (size_t)dec_n, 16000U)) {
                    if (!badge_rx_audio_start_gate_holding(local_now_ms)) {
                        s_recovery_host_underrun_count++;
                    }
                }
                dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                s_audio_decode_primed = 1;
                if (!badge_rx_audio_start_gate_holding(local_now_ms)) {
                    s_stats.v4_audio_frames_out++;
                }
                s_last_audio_jitter_apply_local_ms = local_now_ms;
                s_last_presented_audio_timestamp_ms = (uint32_t)frame->playback_ms;
            } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
                int dec_nb;

                if (badge_rx_ensure_pcm_scratch() != 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                if (s_amr_nb_decoder == NULL) {
                    dashcdg_amr_nb_decoder_create(&s_amr_nb_decoder);
                }
                if (s_amr_nb_decoder == NULL) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                dec_nb = dashcdg_amr_nb_decoder_run_native(
                        s_amr_nb_decoder,
                        frame->encoded_bytes,
                        frame->encoded_length,
                        s_amr_pcm48_scratch,
                        160U
                );
                if (dec_nb <= 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_recovery_zero_buffer_count++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                if (!badge_rx_push_mono_resampled_to_karaoke_line(
                            local_now_ms, s_amr_pcm48_scratch, (size_t)dec_nb, 8000U)) {
                    if (!badge_rx_audio_start_gate_holding(local_now_ms)) {
                        s_recovery_host_underrun_count++;
                    }
                }
                dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                s_audio_decode_primed = 1;
                if (!badge_rx_audio_start_gate_holding(local_now_ms)) {
                    s_stats.v4_audio_frames_out++;
                }
                s_last_audio_jitter_apply_local_ms = local_now_ms;
                s_last_presented_audio_timestamp_ms = (uint32_t)frame->playback_ms;
            } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
                int dec_ret;
                size_t pcm_need;

                if (badge_rx_ensure_opus_decoder(frame_ms, frame->encoded_bytes, frame->encoded_length) != 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                if (badge_rx_ensure_pcm_scratch() != 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                pcm_need = (size_t)s_opus_decoder.frame_size * (size_t)s_opus_decoder.channels;
                if (pcm_need > s_pcm_scratch_samples) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
                    continue;
                }
                dec_ret = dashcdg_opus_decode_frame(
                        &s_opus_decoder,
                        frame->encoded_bytes,
                        frame->encoded_length,
                        s_amr_pcm48_scratch,
                        pcm_need
                );
                if (dec_ret <= 0) {
                    dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                    s_stats.v4_audio_decode_fail++;
                    s_last_audio_jitter_apply_local_ms = local_now_ms;
                    badge_rx_audio_push_degraded_mono(local_now_ms, (size_t)s_opus_decoder.frame_size);
                    continue;
                }
                if (s_opus_decoder.channels == 2) {
                    badge_rx_opus_stereo_interleaved_to_mono_inplace(s_amr_pcm48_scratch, (size_t)dec_ret);
                    (void)badge_rx_push_mono_resampled_to_karaoke_line(
                            local_now_ms, s_amr_pcm48_scratch, (size_t)dec_ret, badge_rx_opus_pcm_native_hz());
                    badge_rx_audio_last_mono_store(s_amr_pcm48_scratch, (size_t)dec_ret);
                } else {
                    (void)badge_rx_push_mono_resampled_to_karaoke_line(
                            local_now_ms, s_amr_pcm48_scratch, (size_t)dec_ret, badge_rx_opus_pcm_native_hz());
                    badge_rx_audio_last_mono_store(s_amr_pcm48_scratch, (size_t)dec_ret);
                }
                dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                s_audio_decode_primed = 1;
                if (!badge_rx_audio_start_gate_holding(local_now_ms)) {
                    s_stats.v4_audio_frames_out++;
                }
                s_last_audio_jitter_apply_local_ms = local_now_ms;
                s_last_presented_audio_timestamp_ms = (uint32_t)frame->playback_ms;
            } else {
                dashcdg_audio_jitter_note_applied(s_audio_jb, frame, frame_ms);
                s_stats.v4_audio_unsupported_codec++;
                s_last_audio_jitter_apply_local_ms = local_now_ms;
                s_last_presented_audio_timestamp_ms = (uint32_t)frame->playback_ms;
                /* Keep pipeline audible under codec mismatch instead of hard silence. */
                badge_rx_audio_push_degraded_mono(local_now_ms, badge_rx_degraded_mono_samples());
            }
        } else {
            break;
        }
    }
}

static void badge_rx_drain_v4_audio(uint64_t local_now_ms)
{
    uint32_t guard_steps = BADGE_RX_AUDIO_DRAIN_GUARD;
    uint32_t plc_ring = BADGE_RX_AUDIO_DRAIN_GUARD;

    if (badge_rx_is_audio_only_decode()) {
        guard_steps = BADGE_RX_AUDIO_DRAIN_GUARD_AUDIO_ONLY;
        plc_ring = BADGE_RX_AUDIO_DRAIN_GUARD_AUDIO_ONLY;
    }
    badge_rx_drain_v4_audio_budget(local_now_ms, guard_steps, plc_ring);
}

static void handle_v4_audio_chunk(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    const struct dashcdg_v4_audio_chunk_payload *a;

    if (view == NULL) {
        return;
    }
    a = &view->v4_audio_chunk;
    if (a->encoded_bytes == NULL || a->encoded_length == 0U) {
        return;
    }
    badge_rx_note_stream_media_sequence(a->media_sequence, &s_audio_seq_inited, &s_audio_next_expected,
                                        &s_stats.audio_missing_estimate);
    if (s_last_audio_codec_inited &&
        (a->codec_id != s_last_audio_codec_id || a->audio_profile_id != s_last_audio_profile_id)) {
        /* Keep AMR-WB decoder state from leaking across codec/profile transitions. */
        badge_rx_amr_decoder_reset();
        if (s_audio_jb != NULL) {
            dashcdg_audio_jitter_clear(s_audio_jb);
        }
        s_audio_decode_primed = 0;
        badge_rx_audio_last_mono_reset();
        s_stats.v4_audio_codec_switches++;
    }
    s_last_audio_codec_id = a->codec_id;
    s_last_audio_profile_id = a->audio_profile_id;
    s_last_audio_codec_inited = 1U;
    if (a->codec_id != s_announced_audio_codec_id || a->audio_profile_id != s_announced_audio_profile_id) {
        s_stats.v4_audio_codec_mismatch++;
    }
    s_stats.v4_audio_chunk_rx++;
    if (!s_audio_decode_enabled) {
        return;
    }
    badge_rx_audio_no_dac_watch_note_rx(local_now_ms);
    if (badge_rx_ensure_audio_jitter() != 0) {
        return;
    }
    if (a->codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
        int pch = dashcdg_opus_probe_packet_channels(a->encoded_bytes, a->encoded_length);

        if (pch == 1 || pch == 2) {
            s_opus_pkt_channels = (uint8_t)pch;
        }
    }
    if (!dashcdg_audio_jitter_insert(
                s_audio_jb,
                a->media_sequence,
                a->playback_ms,
                a->frame_ms,
                a->audio_profile_id,
                a->codec_id,
                a->encoded_bytes,
                a->encoded_length,
                1
        )) {
        return;
    }
    /*
     * Do not drain here: `badge_rx_pump_main_fd` holds `s_mtx` for the whole datagram. Deferring
     * decode/PLC until after `xSemaphoreGive` shortens the critical section and lets the next
     * `recvfrom` run sooner (see `s_pump_defer_audio_drain`).
     */
    {
        uint32_t pkt_budget;
        uint32_t plc_budget = BADGE_RX_AUDIO_PLC_FRAMES_PER_CALL;

        if (badge_rx_is_audio_only_decode()) {
            pkt_budget = BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET_AUDIO_ONLY;
            plc_budget = BADGE_RX_AUDIO_PLC_FRAMES_AUDIO_ONLY;
        } else if (s_video_decode_enabled && s_memory_profile != BADGE_RX_MEM_PROFILE_AUDIO_PRIORITY) {
            pkt_budget = BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET;
        } else {
            pkt_budget = BADGE_RX_AUDIO_DRAIN_BUDGET_ON_AUDIO_PACKET_KARAOKE;
        }
        s_pump_defer_audio_max_steps = pkt_budget;
        s_pump_defer_audio_max_plc = plc_budget;
        s_pump_defer_audio_drain = 1U;
    }
}

static int badge_rx_reset_for_new_session_locked(uint64_t local_now_ms)
{
    s_pump_defer_audio_drain = 0U;
    xSemaphoreGive(s_mtx);
    if (s_audio_jb != NULL) {
        dashcdg_audio_jitter_clear(s_audio_jb);
    }
    badge_rx_amr_decoder_reset();
    dashcdg_platform_hw_karaoke_dac_stop();
    s_audio_decode_primed = 0;
    badge_rx_audio_last_mono_reset();
    s_last_audio_jitter_apply_local_ms = 0U;
    s_last_presented_audio_timestamp_ms = 0U;
    s_last_audio_codec_inited = 0U;
    s_playback_paused = 0U;
    badge_rx_audio_start_gate_stall_clear();
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "session reset: mutex re-acquire failed");
        return -1;
    }

    s_cdg_snapshots_applied = 0U;
    s_pre_anchor_palette_mask = 0U;
    badge_rx_v4_anchor_asm_reset();
    memset(s_video_repair_groups, 0, sizeof(s_video_repair_groups));
    s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    if (s_cdg != NULL) {
        dashcdg_cdg_state_init(s_cdg);
        dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
    }
    if (s_jb != NULL) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
    }
    s_jitter_cdg_primed = 0;
    s_last_cdg_apply_local_ms = 0U;
    /* Keep clock estimator continuity across track/session rolls. */
    s_clock_pb_inited = 0U;
    s_clock_last_playback_ms = 0U;
    s_sync_group_mode = BADGE_RX_TX_GROUP_SYNC_MODE_OFF;
    s_sync_group_target_latency_ms = 0U;
    s_sync_group_phase_spread_ms = 0U;
    s_cdg_skip_hold_until_local_ms = 0U;
    s_sync_leader_instance_id_low16 = 0U;
    s_sync_leader_trim_bias_ppm = 0;
    s_sync_leader_last_update_local_ms = 0U;
    /*
     * New session: TX may reset or jump `header.sequence`. If we keep the old wire gap tracker,
     * `wire_missing_estimate` explodes vs `datagrams` and the karaoke HUD loss line pegs at 100%.
     */
    s_wire_seq_inited = 0U;
    s_wire_next_expected = 0ULL;
    s_wire_seen_bitmap = 0ULL;
    s_stats.datagrams = 0ULL;
    s_stats.wire_missing_estimate = 0ULL;
    s_stats.wire_reorder_events = 0U;
    ESP_LOGI(
            TAG,
            "session: baselined wire seq + loss counters (HUD loss %% / v4_rx_stats track this session; "
            "dg=%llu wm=%llu reo=%lu)",
            (unsigned long long)s_stats.datagrams,
            (unsigned long long)s_stats.wire_missing_estimate,
            (unsigned long)s_stats.wire_reorder_events);
    /* New session timeline: drop wall/playback sync until the next v4_clock_sync (matches rx_start). */
    dashcdg_media_clock_init(&s_mclk);
    s_stats.have_clock = 0;
    s_sync_local_ms = 0U;
    s_sync_playback_ms = 0U;
    /* Preserve long-term drift EMA to avoid relearn pops on playlist advance. */
    s_source_idle_last_mark_ms = 0U;
    s_active_session_local_start_ms = local_now_ms;
    return 0;
}

static void handle_session_info(const struct dashcdg_packet_view *view)
{
    int same_session;

    if (s_video_decode_enabled) {
        badge_rx_try_alloc_cdg_jitter();
    }
    s_announced_playout_delay_ms = view->v4_session_info.startup_preroll_ms;
    s_announced_audio_frame_ms = view->v4_session_info.audio_frame_ms;
    s_announced_audio_codec_id = view->v4_session_info.audio_codec_id;
    s_announced_audio_profile_id = view->v4_session_info.audio_profile_id;
    s_announced_audio_sample_rate = view->v4_session_info.audio_sample_rate;
    s_announced_audio_channels = view->v4_session_info.audio_channels;
    if (s_announced_audio_sample_rate == 0U) {
        s_announced_audio_sample_rate = 48000U;
    }
    if (s_announced_audio_channels == 0U) {
        s_announced_audio_channels = 1U;
    }

    (void)badge_rx_ensure_pcm_scratch();

    same_session = (s_active_session_valid &&
                    s_active_session_start_ms == view->v4_session_info.session_start_ms &&
                    memcmp(s_active_song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID) == 0 &&
                    s_active_asset_size == view->v4_session_info.asset_size);
    if (same_session) {
        if (view->v4_session_info.session_start_ms > view->header.sender_time_ms) {
            s_session_epoch_sender_floor_ms = view->v4_session_info.session_start_ms;
        } else {
            s_session_epoch_sender_floor_ms = view->header.sender_time_ms;
        }
        return;
    }

    if (badge_rx_reset_for_new_session_locked(dashcdg_clock_now_ms()) != 0) {
        return;
    }

    s_active_session_start_ms = view->v4_session_info.session_start_ms;
    s_active_asset_size = view->v4_session_info.asset_size;
    memcpy(s_active_song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID);
    s_active_session_valid = 1;
    if (view->v4_session_info.session_start_ms > view->header.sender_time_ms) {
        s_session_epoch_sender_floor_ms = view->v4_session_info.session_start_ms;
    } else {
        s_session_epoch_sender_floor_ms = view->header.sender_time_ms;
    }
    memset(s_stats.song_id, 0, sizeof(s_stats.song_id));
    memcpy(s_stats.song_id, view->v4_session_info.song_id, DASHCDG_MAX_SONG_ID);
    s_stats.song_id[DASHCDG_MAX_SONG_ID] = '\0';
    s_stats.v4_session_count++;
}

static void handle_clock_sync(const struct dashcdg_packet_view *view, uint64_t local_now_ms)
{
    uint32_t startup_state;
    uint16_t leader_id_low16;
    int16_t leader_trim_ppm;
    uint8_t was_paused;

    was_paused = s_playback_paused;
    s_playback_paused = (uint8_t)(((view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0U) ? 1U : 0U);

    if (view->v4_clock_sync.session_start_ms != 0U &&
        s_active_session_valid &&
        s_active_session_start_ms != 0U &&
        view->v4_clock_sync.session_start_ms != s_active_session_start_ms) {
        /*
         * Guard against stale/reordered clock_sync from an older session causing reset loops.
         * We already maintain a sender-time floor from session_info/clock_sync of the active
         * session; if this clock packet lands behind that floor (with reorder slack), ignore it.
         */
        if (s_session_epoch_sender_floor_ms != 0U &&
            view->header.sender_time_ms + BADGE_RX_V4_SESSION_REORDER_SENDER_SLACK_MS <
                    s_session_epoch_sender_floor_ms) {
            ESP_LOGD(
                    TAG,
                    "ignore stale clock epoch sender=%llu floor=%llu active=%llu incoming=%llu",
                    (unsigned long long)view->header.sender_time_ms,
                    (unsigned long long)s_session_epoch_sender_floor_ms,
                    (unsigned long long)s_active_session_start_ms,
                    (unsigned long long)view->v4_clock_sync.session_start_ms);
            return;
        }
        ESP_LOGI(
                TAG,
                "clock epoch change session=%llu->%llu; forcing session reset",
                (unsigned long long)s_active_session_start_ms,
                (unsigned long long)view->v4_clock_sync.session_start_ms);
        if (badge_rx_reset_for_new_session_locked(local_now_ms) != 0) {
            return;
        }
        s_active_session_start_ms = view->v4_clock_sync.session_start_ms;
        s_session_epoch_sender_floor_ms = view->header.sender_time_ms;
        s_active_session_valid = 1;
        s_stats.v4_session_count++;
        s_recovery_zero_buffer_count++;
    } else if (view->v4_clock_sync.session_start_ms != 0U &&
               (!s_active_session_valid || s_active_session_start_ms == 0U)) {
        s_active_session_start_ms = view->v4_clock_sync.session_start_ms;
        s_active_session_local_start_ms = local_now_ms;
        s_session_epoch_sender_floor_ms = view->header.sender_time_ms;
        s_active_session_valid = 1;
    }

    if (s_clock_pb_inited) {
        uint64_t delta_ms = (view->v4_clock_sync.playback_ms >= s_clock_last_playback_ms)
                                ? (view->v4_clock_sync.playback_ms - s_clock_last_playback_ms)
                                : 0U;
        if (delta_ms > ((uint64_t)BADGE_RX_CLOCK_TICK_ESTIMATE_MS * 2U)) {
            uint64_t ticks = delta_ms / (uint64_t)BADGE_RX_CLOCK_TICK_ESTIMATE_MS;
            if (ticks > 1U) {
                s_stats.clock_missing_estimate += (uint32_t)(ticks - 1U);
            }
        }
    } else {
        s_clock_pb_inited = 1U;
    }
    s_clock_last_playback_ms = view->v4_clock_sync.playback_ms;
    startup_state = view->v4_clock_sync.startup_state;
    if ((startup_state & BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MAGIC_MASK) == BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MAGIC) {
        s_sync_group_mode = (uint8_t)((startup_state >> BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MODE_SHIFT) &
                                      BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_MODE_MASK);
        s_sync_group_target_latency_ms = (uint16_t)((startup_state >> BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_TARGET_SHIFT) &
                                                    BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_TARGET_MASK);
        s_sync_group_phase_spread_ms = (uint8_t)((startup_state >> BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_SHIFT) &
                                                 BADGE_RX_V4_CLOCK_SYNC_SYNCCTRL_SPREAD_MASK);
    } else {
        s_sync_group_mode = BADGE_RX_TX_GROUP_SYNC_MODE_ACTIVE;
        s_sync_group_target_latency_ms = 0U;
        s_sync_group_phase_spread_ms = 0U;
    }
    leader_id_low16 = (uint16_t)((view->v4_clock_sync.reserved >> 16U) & 0xffffU);
    leader_trim_ppm = (int16_t)(view->v4_clock_sync.reserved & 0xffffU);
    s_sync_leader_instance_id_low16 = leader_id_low16;
    s_sync_leader_trim_bias_ppm = leader_trim_ppm;
    s_sync_leader_last_update_local_ms = local_now_ms;
    if (s_sync_local_ms != 0U && local_now_ms > s_sync_local_ms && view->v4_clock_sync.playback_ms >= s_sync_playback_ms) {
        uint64_t local_delta = local_now_ms - s_sync_local_ms;
        uint64_t playback_delta = view->v4_clock_sync.playback_ms - s_sync_playback_ms;
        int64_t err_ms = (int64_t)playback_delta - (int64_t)local_delta;
        int32_t ppm = (int32_t)((err_ms * 1000000LL) / (int64_t)local_delta);
        if (ppm > 2000) {
            ppm = 2000;
        } else if (ppm < -2000) {
            ppm = -2000;
        }
        s_sync_drift_trim_ppm_ema = (s_sync_drift_trim_ppm_ema * 7 + ppm) / 8;
    }
    /*
     * Map local receive time to the wire media playback timeline using `playback_ms` from
     * `v4_clock_sync` (same domain as audio/CDG `playback_ms` tags). Do not use `header.sender_time_ms`
     * here — it is TX wall clock and drifts vs media playback under A/V correction and pause.
     */
    {
        int64_t pb = (int64_t)view->v4_clock_sync.playback_ms;

        if (pb < 0) {
            pb = 0;
        }
        dashcdg_media_clock_observe(
                &s_mclk, (int64_t)local_now_ms, pb, BADGE_RX_MEDIA_CLOCK_OBSERVE_MAX_STEP_MS);
    }
    s_sync_local_ms = local_now_ms;
    s_sync_playback_ms = view->v4_clock_sync.playback_ms;
    s_stats.have_clock = 1;
    s_stats.v4_clock_count++;
    if (was_paused && !s_playback_paused) {
        if (s_audio_jb != NULL) {
            dashcdg_audio_jitter_clear(s_audio_jb);
        }
        badge_rx_amr_decoder_reset();
        dashcdg_platform_hw_karaoke_dac_stop();
        s_audio_decode_primed = 0;
        badge_rx_audio_last_mono_reset();
        s_last_audio_jitter_apply_local_ms = 0U;
        s_recovery_host_underrun_count++;
    }
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
    int missing_indices[BADGE_RX_CDG_REPAIR_GROUP_SIZE];
    uint8_t missing_count = 0U;
    uint8_t symbol_bytes;
    uint8_t parity_ids[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
    uint8_t parity_count = 0U;
    uint8_t known[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX];

    if (g == NULL || !g->occupied || g->expected_group_size <= 1U) {
        return;
    }
    if (g->parity_symbol_bytes > BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX) {
        return;
    }
    /*
     * Missing members + NACK do not require parity yet; parity arrives on the repair port (24686)
     * when TX uses split. Previously parity_symbol_bytes==0 returned here so NACK/repair counters
     * stayed at zero forever on the badge.
     */
    for (uint8_t i = 0U; i < g->expected_group_size; ++i) {
        if (!g->member_present[i] && missing_count < BADGE_RX_CDG_REPAIR_GROUP_SIZE) {
            missing_indices[missing_count++] = (int)i;
        }
    }
    if (missing_count > 0U) {
        uint16_t missing_mask = 0U;
        for (uint8_t m = 0U; m < missing_count; ++m) {
            if (missing_indices[m] >= 0 && missing_indices[m] < 16) {
                missing_mask |= (uint16_t)(1U << (uint8_t)missing_indices[m]);
            }
        }
        if (missing_mask != 0U && s_repair_nack_enabled != 0U) {
            badge_rx_send_v4_repair_nack(dashcdg_clock_now_ms(), g->group_id, g->expected_group_size, missing_mask);
        }
    }
    if (g->parity_symbol_bytes == 0U) {
        return;
    }
    symbol_bytes = g->parity_symbol_bytes;
    for (uint8_t pi = 0U; pi < BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX; ++pi) {
        if ((g->parity_present_mask & (uint8_t)(1U << pi)) != 0U) {
            parity_ids[parity_count++] = pi;
            memset(known[pi], 0, symbol_bytes);
        }
    }
    for (uint8_t i = 0U; i < g->expected_group_size; ++i) {
        if (g->member_present[i]) {
            uint8_t sym[BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX];
            badge_rx_video_repair_make_symbol(g->member_payloads[i], g->member_lengths[i], sym, symbol_bytes);
            for (uint8_t p = 0U; p < parity_count; ++p) {
                uint8_t pid = parity_ids[p];
                uint8_t coeff = badge_rx_video_repair_coeff(i, pid);
                for (uint8_t b = 0U; b < symbol_bytes; ++b) {
                    known[pid][b] ^= badge_rx_gf256_mul(coeff, sym[b]);
                }
            }
        }
    }
    if (missing_count == 0U || parity_count < missing_count || missing_count > BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
        return;
    }
    {
        uint8_t coeff_matrix[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t rhs[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t sol[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX];
        uint8_t rec_syms[BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX][BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX];
        memset(rec_syms, 0, sizeof(rec_syms));
        for (uint8_t b = 0U; b < symbol_bytes; ++b) {
            for (uint8_t r = 0U; r < missing_count; ++r) {
                uint8_t pid = parity_ids[r];
                rhs[r] = (uint8_t)(g->parity_symbols[pid][b] ^ known[pid][b]);
                for (uint8_t c = 0U; c < missing_count; ++c) {
                    coeff_matrix[r][c] = badge_rx_video_repair_coeff((uint8_t)missing_indices[c], pid);
                }
            }
            if (!badge_rx_gf256_solve(missing_count, coeff_matrix, rhs, sol)) {
                s_stats.v4_video_repair_failed++;
                return;
            }
            for (uint8_t c = 0U; c < missing_count; ++c) {
                rec_syms[c][b] = sol[c];
            }
        }
        for (uint8_t c = 0U; c < missing_count; ++c) {
            int mi = missing_indices[c];
            uint16_t recovered_length = rec_syms[c][0];
            uint8_t packet_count;
            uint64_t batch_index;
            uint64_t packet_start_index;
            uint64_t tail;
            if (recovered_length == 0U || recovered_length > BADGE_RX_VIDEO_REPAIR_PAYLOAD_MAX ||
                recovered_length > (uint16_t)(symbol_bytes - 1U) ||
                (recovered_length % DASHCDG_SUBCHANNEL_PACKET_BYTES) != 0U) {
                s_stats.v4_video_repair_failed++;
                return;
            }
            packet_count = (uint8_t)(recovered_length / DASHCDG_SUBCHANNEL_PACKET_BYTES);
            batch_index = (uint64_t)g->group_id * (uint64_t)BADGE_RX_CDG_REPAIR_GROUP_SIZE + (uint64_t)mi;
            packet_start_index = batch_index * (uint64_t)DASHCDG_MAX_CDG_BATCH_PACKETS;
            if (packet_count == 0U || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS || s_jb == NULL) {
                s_stats.v4_video_repair_failed++;
                return;
            }
            /*
             * Wi‑Fi often delivers repair after the jitter cursor has stepped past the gap. Insert then
             * rejects (tail < next_packet_index). For a single missing batch, rewind once with
             * apply_snapshot_seek (same contract as anchor seek / drain gap-fill). Multi-miss skips this:
             * one seek could clear an earlier recovered batch in the same group.
             */
            if (missing_count == 1U && s_jb->initialized) {
                tail = packet_start_index + (uint64_t)packet_count;
                if (tail < s_jb->next_packet_index) {
                    const uint64_t gap_packets = s_jb->next_packet_index - tail;
                    const uint64_t max_gap_packets = (uint64_t)DASHCDG_MAX_CDG_BATCH_PACKETS * 8U;
                    if (gap_packets <= max_gap_packets && dashcdg_cdg_batch_jitter_occupied_count(s_jb) <= 12U) {
                        dashcdg_cdg_batch_jitter_apply_snapshot_seek(s_jb, packet_start_index);
                    }
                }
            }
            if (!dashcdg_cdg_batch_jitter_insert(s_jb, packet_start_index, packet_count, &rec_syms[c][1], 0)) {
                s_stats.v4_video_repair_failed++;
                return;
            }
            g->member_present[mi] = 1U;
            g->member_lengths[mi] = recovered_length;
            memcpy(g->member_payloads[mi], &rec_syms[c][1], recovered_length);
            s_stats.v4_video_repair_recovered++;
        }
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
    size_t occ;
    size_t cap = 0U;
    dashcdg_cdg_jb_insert_rc ins_rc;
    size_t evict_goal;

    if (view->v4_video_delta.delta_format != DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS) {
        return;
    }
    badge_rx_note_stream_media_sequence(view->v4_video_delta.media_sequence, &s_cdg_seq_inited, &s_cdg_next_expected,
                                        &s_stats.cdg_missing_estimate);
    if (s_jb == NULL) {
        return;
    }

    occ = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
    cap = dashcdg_cdg_batch_jitter_capacity(s_jb);

    /*
     * Do not call drain_cdg_to_idle() here: this handler runs with `s_mtx` held (media pump). A full
     * CDG+audio drain in the locked packet path can stall recv/select long enough to starve the stream
     * and wedge diagnostics. Evict furthest-ahead batches first; successful insert still drains below.
     */
    if (cap > 0U && occ + s_cdg_jb_headroom_slots > cap) {
        dashcdg_cdg_batch_jitter_evict_pressure(s_jb, s_cdg_jb_headroom_slots);
        s_stats.jb_evict_rounds++;
        if (s_cdg_blit_max_y > BADGE_RX_CDG_BLIT_PARTIAL_H) {
            s_cdg_blit_max_y = (uint16_t)BADGE_RX_CDG_BLIT_PARTIAL_H;
        }
    }

    ins_rc = dashcdg_cdg_batch_jitter_try_insert(s_jb, view->v4_video_delta.packet_start_index, view->v4_video_delta.packet_count,
                                                 view->v4_video_delta.delta_bytes, 1);
    if (ins_rc == DASHCDG_CDG_JB_INSERT_OK) {
        badge_rx_video_repair_observe_delta(view->v4_video_delta.group_id, view->v4_video_delta.group_index,
                                            view->v4_video_delta.delta_bytes, view->v4_video_delta.encoded_length);
        s_stats.v4_video_delta_count++;
        drain_cdg_to_idle(local_now_ms);
        return;
    }
    if (ins_rc == DASHCDG_CDG_JB_INSERT_DUP) {
        s_stats.cdg_delta_insert_dup++;
        return;
    }
    if (ins_rc == DASHCDG_CDG_JB_INSERT_STALE) {
        s_stats.cdg_delta_insert_stale++;
        return;
    }

    evict_goal = s_cdg_jb_headroom_slots + 2U;
    if (evict_goal < 4U) {
        evict_goal = 4U;
    }
    if (cap > 0U && evict_goal > cap) {
        evict_goal = cap;
    }
    dashcdg_cdg_batch_jitter_evict_pressure(s_jb, evict_goal);
    s_stats.jb_evict_rounds++;
    if (s_cdg_blit_max_y > BADGE_RX_CDG_BLIT_PARTIAL_H) {
        s_cdg_blit_max_y = (uint16_t)BADGE_RX_CDG_BLIT_PARTIAL_H;
    }
    ins_rc = dashcdg_cdg_batch_jitter_try_insert(s_jb, view->v4_video_delta.packet_start_index, view->v4_video_delta.packet_count,
                                                 view->v4_video_delta.delta_bytes, 1);
    if (ins_rc == DASHCDG_CDG_JB_INSERT_OK) {
        badge_rx_video_repair_observe_delta(view->v4_video_delta.group_id, view->v4_video_delta.group_index,
                                            view->v4_video_delta.delta_bytes, view->v4_video_delta.encoded_length);
        s_stats.v4_video_delta_count++;
        drain_cdg_to_idle(local_now_ms);
        return;
    }
    if (ins_rc == DASHCDG_CDG_JB_INSERT_DUP) {
        s_stats.cdg_delta_insert_dup++;
        return;
    }
    if (ins_rc == DASHCDG_CDG_JB_INSERT_STALE) {
        s_stats.cdg_delta_insert_stale++;
        return;
    }
    if (ins_rc != DASHCDG_CDG_JB_INSERT_BAD_ARGS) {
        s_stats.cdg_delta_insert_fail++;
        if ((s_stats.cdg_delta_insert_fail & 31U) == 1U) {
            ESP_LOGW(TAG, "CDG jitter ring_full after evict (cap=%u occ=%u ps=%llu)", (unsigned)cap,
                     (unsigned)dashcdg_cdg_batch_jitter_occupied_count(s_jb),
                     (unsigned long long)view->v4_video_delta.packet_start_index);
        }
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
    {
        static uint8_t s_repair_group_inited;
        static uint32_t s_repair_group_id;
        static uint16_t s_repair_seen_mask;
        if (!s_repair_group_inited || rw->group_id != s_repair_group_id) {
            if (s_repair_group_inited && rw->group_id > s_repair_group_id) {
                uint8_t expected = 1U;
                uint16_t kbits = (uint16_t)((rw->reserved & DASHCDG_V4_REPAIR_WINDOW_RESERVED_K_MASK) >>
                                            DASHCDG_V4_REPAIR_WINDOW_RESERVED_K_SHIFT);
                if (kbits > 0U) {
                    expected = (uint8_t)kbits;
                }
                uint8_t seen = 0U;
                for (uint8_t i = 0U; i < expected; ++i) {
                    if ((s_repair_seen_mask & (uint16_t)(1U << i)) != 0U) {
                        seen++;
                    }
                }
                if (seen < expected) {
                    s_stats.repair_missing_estimate += (uint32_t)(expected - seen);
                }
            }
            s_repair_group_inited = 1U;
            s_repair_group_id = rw->group_id;
            s_repair_seen_mask = 0U;
        }
        if (rw->redundancy_index < BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
            s_repair_seen_mask |= (uint16_t)(1U << rw->redundancy_index);
        }
    }
    dir = (uint16_t)(rw->reserved & DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_MASK);
    if (dir == DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_FORWARD) {
        s_stats.v4_video_repair_rx_forward++;
    } else if (dir == DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_REVERSE) {
        s_stats.v4_video_repair_rx_reverse++;
    }
    g = badge_rx_video_repair_get_group(rw->group_id);
    if (g == NULL || rw->group_size <= 1U || rw->group_size > BADGE_RX_CDG_REPAIR_GROUP_SIZE || rw->payload_length == 0U ||
        rw->payload_length > BADGE_RX_VIDEO_REPAIR_SYMBOL_MAX || rw->payload_bytes == NULL) {
        return;
    }
    if (g->expected_group_size == 0U || g->expected_group_size > rw->group_size) {
        g->expected_group_size = rw->group_size;
    }
    if (g->parity_symbol_bytes != (uint8_t)rw->payload_length) {
        g->parity_present_mask = 0U;
        g->parity_symbol_bytes = (uint8_t)rw->payload_length;
    }
    if (rw->redundancy_index < BADGE_RX_VIDEO_REPAIR_REDUNDANCY_MAX) {
        uint8_t ridx = rw->redundancy_index;
        memset(g->parity_symbols[ridx], 0, sizeof(g->parity_symbols[ridx]));
        memcpy(g->parity_symbols[ridx], rw->payload_bytes, g->parity_symbol_bytes);
        g->parity_present_mask |= (uint8_t)(1U << ridx);
    } else {
        return;
    }
    badge_rx_video_repair_try_recover_group(g);
}

/** Repair/FEC datagrams on BADGE_RX_REPAIR_PORT (not counted in main `datagrams` / sequence loss). */
static void badge_rx_process_repair_datagram(const uint8_t *buf, size_t buflen)
{
    struct dashcdg_packet_view view;

    if (!dashcdg_protocol_parse_packet(&view, buf, buflen)) {
        return;
    }
    if (view.header.type == DASHCDG_PACKET_V4_REPAIR_WINDOW && s_video_decode_enabled != 0U) {
        handle_video_repair_window(&view);
    }
}

static void rx_one_datagram(uint8_t *buf, size_t buflen, uint64_t local_now_ms)
{
    struct dashcdg_packet_view view;
    uint64_t seq;

    if (!dashcdg_protocol_parse_packet(&view, buf, buflen)) {
        s_stats.parse_failures++;
        return;
    }

    seq = view.header.sequence;
    s_stats.last_sequence = seq;
    badge_rx_note_wire_sequence(seq);
    badge_rx_note_media_sequence_duplicate(seq);
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
        /* `drain_cdg_to_idle` walks CDG jitter + audio; skip CDG drain when video decode is off. */
        if (s_video_decode_enabled) {
            drain_cdg_to_idle(local_now_ms);
        } else {
            badge_rx_drain_v4_audio(local_now_ms);
        }
        break;
    case DASHCDG_PACKET_V4_LOADING_SCREEN:
        badge_rx_handle_v4_loading_screen(&view, local_now_ms);
        break;
    case DASHCDG_PACKET_V4_VIDEO_DELTA:
        if (badge_rx_is_stale_prior_session_media(&view)) {
            s_stats.cdg_missing_estimate++;
            break;
        }
        if (s_video_decode_enabled) {
            handle_video_delta(&view, local_now_ms);
        }
        break;
    case DASHCDG_PACKET_V4_REPAIR_WINDOW:
        if (s_video_decode_enabled) {
            handle_video_repair_window(&view);
        }
        break;
    case DASHCDG_PACKET_V4_VIDEO_ANCHOR:
        if (badge_rx_is_stale_prior_session_media(&view)) {
            s_stats.v4_anchor_rejected_behind++;
            break;
        }
        if (s_video_decode_enabled) {
            s_stats.v4_anchor_chunks++;
            badge_rx_handle_v4_video_anchor(&view, local_now_ms);
        }
        break;
    case DASHCDG_PACKET_V4_AUDIO_CHUNK:
        if (badge_rx_is_stale_prior_session_audio(&view)) {
            s_stats.audio_missing_estimate++;
            break;
        }
        handle_v4_audio_chunk(&view, local_now_ms);
        break;
    default:
        break;
    }
}

static void badge_rx_deferred_audio_drain_if_pending_locked(uint64_t local_now_ms)
{
    uint32_t st;
    uint32_t plc;
    size_t occ;

    if (s_pump_defer_audio_drain == 0U) {
        return;
    }
    st = s_pump_defer_audio_max_steps;
    plc = s_pump_defer_audio_max_plc;
    /*
     * Real backlog: if we only ever drain 8 steps per UDP datagram while Wi‑Fi delivers bursts, the
     * jitter ring pegs full (karaoke HUD `a100`) even though the link is not "stats broken" — we are
     * simply not stepping playout fast enough between inserts. Scale the **post-recv** deferred drain
     * when occupancy is high (still capped; insert path stays tiny).
     */
    if (badge_rx_is_audio_only_decode() && s_audio_jb != NULL && s_audio_jb->initialized) {
        occ = dashcdg_audio_jitter_occupied_count(s_audio_jb);
        if (occ >= 48U) {
            st = BADGE_RX_AUDIO_DRAIN_DEFERRED_MAX_STEPS_AUDIO_ONLY;
        } else if (occ >= 36U) {
            if (st < 28U) {
                st = 28U;
            }
        } else if (occ >= 24U) {
            if (st < 20U) {
                st = 20U;
            }
        } else if (occ >= 16U) {
            if (st < 14U) {
                st = 14U;
            }
        }
        if (st > BADGE_RX_AUDIO_DRAIN_DEFERRED_MAX_STEPS_AUDIO_ONLY) {
            st = BADGE_RX_AUDIO_DRAIN_DEFERRED_MAX_STEPS_AUDIO_ONLY;
        }
        if (occ >= 24U && plc < BADGE_RX_AUDIO_PLC_FRAMES_AUDIO_ONLY) {
            plc = BADGE_RX_AUDIO_PLC_FRAMES_AUDIO_ONLY;
        }
    }
    s_pump_defer_audio_drain = 0U;
    badge_rx_drain_v4_audio_budget(local_now_ms, st, plc);
}

static void badge_rx_pump_flush_deferred_audio_drain(uint64_t local_now_ms)
{
    TickType_t mtx_ticks = BADGE_RX_PUMP_MUTEX_TICKS;

    if (s_pump_defer_audio_drain == 0U) {
        return;
    }
    if (xSemaphoreTake(s_mtx, mtx_ticks) != pdTRUE) {
        s_stats.rx_mtx_pump_timeouts++;
        if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
            return;
        }
    }
    badge_rx_deferred_audio_drain_if_pending_locked(local_now_ms);
    xSemaphoreGive(s_mtx);
}

/** Full wire parse + session handlers (media port: multicast and/or STA unicast duplicate bind). */
static void badge_rx_pump_main_fd(int pump_fd, uint8_t *buf, size_t buf_cap)
{
    for (;;) {
        struct sockaddr_in src;
        socklen_t srclen = (socklen_t)sizeof(src);
        ssize_t n = recvfrom(pump_fd, buf, buf_cap, MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);

        if (n <= 0) {
            break;
        }
        {
            const uint64_t now_ms = dashcdg_clock_now_ms();

            badge_rx_pump_flush_deferred_audio_drain(now_ms);
            if (pump_fd == (int)s_sock) {
                s_rx_mcast_media_datagrams++;
                s_last_mcast_media_rx_ms = now_ms;
            } else if (pump_fd == (int)s_ucast_media_sock) {
                s_rx_ucast_media_datagrams++;
                s_last_ucast_media_rx_ms = now_ms;
            }
            {
                TickType_t mtx_ticks = BADGE_RX_PUMP_MUTEX_TICKS;

                if (xSemaphoreTake(s_mtx, mtx_ticks) != pdTRUE) {
                    s_stats.rx_mtx_pump_timeouts++;
                    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
                        continue;
                    }
                }
                if (src.sin_family == AF_INET && badge_rx_ipv4_is_unicast_src(src.sin_addr.s_addr)) {
                    s_v4_tx_src_ipv4 = src.sin_addr.s_addr;
                }
                {
#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
                    const int64_t t_hold0 = esp_timer_get_time();
#endif
                    s_stats.datagrams++;
                    rx_one_datagram(buf, (size_t)n, now_ms);
#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
                    {
                        int64_t dt_hold = esp_timer_get_time() - t_hold0;

                        if (dt_hold > 0 && dt_hold < 5000000) {
                            badge_rx_perf_note_mtx_pump_us((uint32_t)dt_hold);
                        }
                    }
#endif
                    xSemaphoreGive(s_mtx);
                    badge_rx_pump_flush_deferred_audio_drain(now_ms);
                    dashcdg_platform_hw_note_karaoke_mcast_rx(now_ms);
                }
            }
        }
    }
}

static void badge_rx_close_ucast_sockets(void)
{
    int x;

    x = (int)s_ucast_media_sock;
    s_ucast_media_sock = -1;
    if (x >= 0) {
        close(x);
    }
    x = (int)s_ucast_stats_sock;
    s_ucast_stats_sock = -1;
    if (x >= 0) {
        close(x);
    }
    x = (int)s_ucast_repair_sock;
    s_ucast_repair_sock = -1;
    if (x >= 0) {
        close(x);
    }
}

static uint8_t badge_rx_ucast_mask_from_fds(void)
{
    uint8_t m = 0U;

    if ((int)s_ucast_media_sock >= 0) {
        m |= DASHCDG_BADGE_UCAST_RX_MASK_MEDIA;
    }
    if ((int)s_ucast_stats_sock >= 0) {
        m |= DASHCDG_BADGE_UCAST_RX_MASK_STATS;
    }
    if ((int)s_ucast_repair_sock >= 0) {
        m |= DASHCDG_BADGE_UCAST_RX_MASK_REPAIR;
    }
    return m;
}

static size_t badge_rx_internal_largest_block_bytes(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static int badge_rx_heap_ok_for_optional_ucast_dup(void)
{
    return badge_rx_internal_largest_block_bytes() >= (size_t)BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_OPTIONAL_BYTES;
}

static int badge_rx_open_ucast_rx_on_port(uint16_t port, int rcv_bytes)
{
    int s;
    struct sockaddr_in addr;
    int reuse = 1;
    int rcv = rcv_bytes;
    esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;

    if (na == NULL || esp_netif_get_ip_info(na, &ipi) != ESP_OK || ipi.ip.addr == 0) {
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        return -1;
    }
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv)) != 0) {
        ESP_LOGW(TAG, "ucast port %u SO_RCVBUF=%d errno=%d", (unsigned)port, rcv, errno);
    }
    badge_rx_sock_set_dashcdg_dscp(s, "ucast_rx");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ipi.ip.addr;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "ucast bind port %u errno=%d", (unsigned)port, errno);
        close(s);
        return -1;
    }
    return s;
}

/** Open STA-bound duplicates when DHCP has an address (retried from RX idle if early). */
static void badge_rx_try_open_ucast_sockets(void)
{
    if ((int)s_ucast_media_sock < 0) {
        if (badge_rx_internal_largest_block_bytes() < (size_t)BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_MEDIA_CRITICAL) {
            static uint64_t s_last_ucast_media_crit_ms;
            const uint64_t now_ms = dashcdg_clock_now_ms();

            if (s_last_ucast_media_crit_ms == 0U || now_ms - s_last_ucast_media_crit_ms >= 8000U) {
                s_last_ucast_media_crit_ms = now_ms;
                ESP_LOGW(
                        TAG,
                        "ucast media dup blocked internal_largest=%u need>=%u",
                        (unsigned)badge_rx_internal_largest_block_bytes(),
                        (unsigned)BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_MEDIA_CRITICAL
                );
            }
        } else {
            int s = badge_rx_open_ucast_rx_on_port((uint16_t)BADGE_RX_PORT, BADGE_RX_RCVBUF_MEDIA);

            if (s >= 0) {
                s_ucast_media_sock = s;
                ESP_LOGI(TAG, "unicast RX dup media port %d fd=%d", (int)BADGE_RX_PORT, s);
            }
        }
    }
    {
        const uint64_t now_ms = dashcdg_clock_now_ms();

        if (now_ms < s_ucast_optional_earliest_ms) {
            return;
        }
    }
    if (!badge_rx_heap_ok_for_optional_ucast_dup()) {
        static uint64_t s_last_ucast_optional_skip_ms;
        const uint64_t now_ms = dashcdg_clock_now_ms();

        if (((int)s_stats_sock >= 0 && (int)s_ucast_stats_sock < 0) ||
                ((int)s_repair_sock >= 0 && (int)s_ucast_repair_sock < 0)) {
            if (s_last_ucast_optional_skip_ms == 0U || now_ms - s_last_ucast_optional_skip_ms >= 8000U) {
                s_last_ucast_optional_skip_ms = now_ms;
                ESP_LOGW(
                        TAG,
                        "ucast stats/repair dup deferred internal_largest=%u need>=%u",
                        (unsigned)badge_rx_internal_largest_block_bytes(),
                        (unsigned)BADGE_RX_MIN_LARGEST_INTERNAL_FOR_UCAST_OPTIONAL_BYTES
                );
            }
        }
        return;
    }
    if ((int)s_stats_sock >= 0 && (int)s_ucast_stats_sock < 0) {
        int s = badge_rx_open_ucast_rx_on_port((uint16_t)BADGE_RX_TX_STATS_PORT, BADGE_RX_RCVBUF_CONTROL);

        if (s >= 0) {
            s_ucast_stats_sock = s;
            ESP_LOGI(TAG, "unicast RX dup stats port %d fd=%d", (int)BADGE_RX_TX_STATS_PORT, s);
        }
    }
    if ((int)s_repair_sock >= 0 && (int)s_ucast_repair_sock < 0) {
        int s = badge_rx_open_ucast_rx_on_port((uint16_t)BADGE_RX_REPAIR_PORT, BADGE_RX_RCVBUF_CONTROL);

        if (s >= 0) {
            s_ucast_repair_sock = s;
            ESP_LOGI(TAG, "unicast RX dup repair port %d fd=%d", (int)BADGE_RX_REPAIR_PORT, s);
        }
    }
}

static void badge_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[DASHCDG_MAX_PACKET_SIZE];

    while (s_run) {
#if defined(ESP_PLATFORM)
        /*
         * Something (NVS default, coexist, or a race with screen PM) can re-enable modem PS while RX
         * is active — multicast delivery suffers. Re-clamp periodically; no-op when already NONE.
         * (1.5 s: faster recovery than 5 s if the stack toggles PS after Wi-Fi events.)
         */
        {
            static uint64_t s_last_wifi_ps_clamp_ms;
            uint64_t t = dashcdg_clock_now_ms();

            if (s_last_wifi_ps_clamp_ms == 0U || t - s_last_wifi_ps_clamp_ms >= 1500U) {
                s_last_wifi_ps_clamp_ms = t;
                (void)esp_wifi_set_ps(WIFI_PS_NONE);
            }
        }
#endif
        fd_set rfds;
        struct timeval tv;
        int rv;
        int fd = (int)s_sock;
        int stats_fd = (int)s_stats_sock;
        int repair_fd = (int)s_repair_sock;
        int ucast_m = (int)s_ucast_media_sock;
        int ucast_s = (int)s_ucast_stats_sock;
        int ucast_r = (int)s_ucast_repair_sock;
        int max_fd = -1;

        FD_ZERO(&rfds);
        if (fd >= 0) {
            FD_SET(fd, &rfds);
            max_fd = fd;
        }
        if (stats_fd >= 0) {
            FD_SET(stats_fd, &rfds);
            if (stats_fd > max_fd) {
                max_fd = stats_fd;
            }
        }
        if (repair_fd >= 0) {
            FD_SET(repair_fd, &rfds);
            if (repair_fd > max_fd) {
                max_fd = repair_fd;
            }
        }
        if (ucast_m >= 0) {
            FD_SET(ucast_m, &rfds);
            if (ucast_m > max_fd) {
                max_fd = ucast_m;
            }
        }
        if (ucast_s >= 0) {
            FD_SET(ucast_s, &rfds);
            if (ucast_s > max_fd) {
                max_fd = ucast_s;
            }
        }
        if (ucast_r >= 0) {
            FD_SET(ucast_r, &rfds);
            if (ucast_r > max_fd) {
                max_fd = ucast_r;
            }
        }
        if (max_fd < 0) {
            break;
        }
#if defined(FD_SETSIZE)
        if (max_fd >= FD_SETSIZE) {
            static uint64_t s_last_fdset_warn_ms;
            const uint64_t now_ms = dashcdg_clock_now_ms();

            if (s_last_fdset_warn_ms == 0U || now_ms - s_last_fdset_warn_ms >= 5000U) {
                s_last_fdset_warn_ms = now_ms;
                ESP_LOGW(
                        TAG,
                        "select: max_fd=%d >= FD_SETSIZE=%d — closing ucast dup sockets",
                        max_fd,
                        FD_SETSIZE
                );
            }
            badge_rx_close_ucast_sockets();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
#endif
        tv.tv_sec = 0;
        {
            uint32_t sel_ms = BADGE_RX_SELECT_TIMEOUT_MS;

            /*
             * Kconfig `DASHCDG_BADGE_RX_SELECT_TIMEOUT_*` (T8). Shorter audio-only wake helps DAC + Opus drain;
             * karaoke entry forces `WIFI_PS_NONE` in platform_hw so IGMP/mcast is not starved by modem sleep.
             */
            if (s_audio_decode_enabled != 0U && s_video_decode_enabled == 0U) {
                sel_ms = BADGE_RX_SELECT_TIMEOUT_AUDIO_ONLY_MS;
            }
            tv.tv_usec = (int)(sel_ms * 1000U);
        }
        rv = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EBADF || errno == EINVAL || !s_run) {
                break;
            }
            if (errno == ENOMEM) {
                const uint64_t now_ms = dashcdg_clock_now_ms();

                if (s_last_select_enomem_local_ms == 0U || now_ms - s_last_select_enomem_local_ms >= 2000U) {
                    s_last_select_enomem_local_ms = now_ms;
                    ESP_LOGW(
                            TAG,
                            "select: ENOMEM (max_fd=%d) internal_free=%u internal_largest=%u — "
                            "closing ucast dup, backoff",
                            max_fd,
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                    );
                }
                badge_rx_close_ucast_sockets();
                {
                    uint64_t cool = dashcdg_clock_now_ms() + 5000ULL;

                    if (cool > s_ucast_optional_earliest_ms) {
                        s_ucast_optional_earliest_ms = cool;
                    }
                }
                /* lwIP + newlib may need more than one tick to reclaim FD/path state after ENOMEM. */
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
            ESP_LOGW(TAG, "select: errno=%d", errno);
            continue;
        }
        if (rv == 0) {
            const uint64_t idle_now_ms = dashcdg_clock_now_ms();
            /* Audio decode needs periodic drain even when the mutex is busy (large UDP bursts). */
            TickType_t idle_mtx_ticks = pdMS_TO_TICKS(12);

            if (s_audio_decode_enabled != 0U) {
                /* Audio-only: wait longer for `s_mtx` — missing idle drain was baseline chop (RX vs LVGL). */
                idle_mtx_ticks = badge_rx_is_audio_only_decode() ? pdMS_TO_TICKS(200) : pdMS_TO_TICKS(56);
            }

            badge_rx_try_open_ucast_sockets();
            if (xSemaphoreTake(s_mtx, idle_mtx_ticks) == pdTRUE) {
                badge_rx_deferred_audio_drain_if_pending_locked(idle_now_ms);
                if (s_stats.have_clock &&
                    s_last_audio_jitter_apply_local_ms != 0U &&
                    idle_now_ms > s_last_audio_jitter_apply_local_ms &&
                    (idle_now_ms - s_last_audio_jitter_apply_local_ms) > 1000U &&
                    (s_source_idle_last_mark_ms == 0U || (idle_now_ms - s_source_idle_last_mark_ms) > 1000U)) {
                    s_source_idle_park_count++;
                    s_source_idle_last_mark_ms = idle_now_ms;
                }
                /*
                 * CDG+audio: `drain_cdg_to_idle` leads with bounded audio then walks CDG jitter.
                 * Audio-only / video-off must not enter CDG drain here (jb may be detached).
                 */
                if (s_video_decode_enabled != 0U) {
                    drain_cdg_to_idle(idle_now_ms);
                } else {
                    badge_rx_drain_v4_audio(idle_now_ms);
                }
                xSemaphoreGive(s_mtx);
                badge_rx_adapt_jitter_capacity_unblocked(idle_now_ms);
            } else if (badge_rx_is_audio_only_decode()) {
                s_stats.rx_mtx_idle_drain_miss++;
            }
            dashcdg_platform_hw_karaoke_dac_pad_partial_chunk();
            badge_rx_maybe_send_v4_stats(idle_now_ms);
            badge_rx_maybe_uart_log_audio_stats(idle_now_ms);
            badge_rx_maybe_periodic_igmp_refresh(idle_now_ms);
            continue;
        }
        if (fd >= 0 && FD_ISSET(fd, &rfds)) {
            uint64_t now_ms = dashcdg_clock_now_ms();
            if (badge_rx_media_path_allow_fd(fd, now_ms)) {
                badge_rx_pump_main_fd(fd, buf, sizeof(buf));
            }
        }
        if (ucast_m >= 0 && FD_ISSET(ucast_m, &rfds)) {
            uint64_t now_ms = dashcdg_clock_now_ms();
            if (badge_rx_media_path_allow_fd(ucast_m, now_ms)) {
                badge_rx_pump_main_fd(ucast_m, buf, sizeof(buf));
            }
        }
        if (stats_fd >= 0 && FD_ISSET(stats_fd, &rfds)) {
            for (;;) {
                struct sockaddr_in src;
                socklen_t srclen = (socklen_t)sizeof(src);
                ssize_t n = recvfrom(stats_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);
                struct dashcdg_packet_view view;

                if (n <= 0) {
                    break;
                }
                if (!dashcdg_protocol_parse_packet(&view, buf, (size_t)n)) {
                    continue;
                }
                if (view.header.type == DASHCDG_PACKET_V4_RX_STATS &&
                    view.v4_rx_stats.receiver_instance_id != s_badge_receiver_instance_id) {
                    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
                        s_stats.v4_rx_stats_peer_packets++;
                        xSemaphoreGive(s_mtx);
                    }
                }
            }
        }
        if (ucast_s >= 0 && FD_ISSET(ucast_s, &rfds)) {
            for (;;) {
                struct sockaddr_in src;
                socklen_t srclen = (socklen_t)sizeof(src);
                ssize_t n = recvfrom(ucast_s, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);
                struct dashcdg_packet_view view;

                if (n <= 0) {
                    break;
                }
                if (!dashcdg_protocol_parse_packet(&view, buf, (size_t)n)) {
                    continue;
                }
                if (view.header.type == DASHCDG_PACKET_V4_RX_STATS &&
                    view.v4_rx_stats.receiver_instance_id != s_badge_receiver_instance_id) {
                    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
                        s_stats.v4_rx_stats_peer_packets++;
                        xSemaphoreGive(s_mtx);
                    }
                }
            }
        }
        if (repair_fd >= 0 && FD_ISSET(repair_fd, &rfds)) {
            for (;;) {
                struct sockaddr_in src;
                socklen_t srclen = (socklen_t)sizeof(src);
                ssize_t n = recvfrom(repair_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);

                if (n <= 0) {
                    break;
                }
                if (xSemaphoreTake(s_mtx, BADGE_RX_REPAIR_MUTEX_TICKS) != pdTRUE) {
                    s_stats.rx_mtx_repair_timeouts++;
                    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
                        continue;
                    }
                }
                if (src.sin_family == AF_INET && badge_rx_ipv4_is_unicast_src(src.sin_addr.s_addr)) {
                    s_v4_tx_src_ipv4 = src.sin_addr.s_addr;
                }
                badge_rx_process_repair_datagram(buf, (size_t)n);
                xSemaphoreGive(s_mtx);
            }
        }
        if (ucast_r >= 0 && FD_ISSET(ucast_r, &rfds)) {
            for (;;) {
                struct sockaddr_in src;
                socklen_t srclen = (socklen_t)sizeof(src);
                ssize_t n = recvfrom(ucast_r, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);

                if (n <= 0) {
                    break;
                }
                if (xSemaphoreTake(s_mtx, BADGE_RX_REPAIR_MUTEX_TICKS) != pdTRUE) {
                    s_stats.rx_mtx_repair_timeouts++;
                    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
                        continue;
                    }
                }
                if (src.sin_family == AF_INET && badge_rx_ipv4_is_unicast_src(src.sin_addr.s_addr)) {
                    s_v4_tx_src_ipv4 = src.sin_addr.s_addr;
                }
                badge_rx_process_repair_datagram(buf, (size_t)n);
                xSemaphoreGive(s_mtx);
            }
        }
        {
            const uint64_t post_burst_ms = dashcdg_clock_now_ms();
            /*
             * Post-burst drain must acquire `s_mtx` after socket work. Audio-only needs long patience
             * (LVGL); A/V used to use 12 ms and often missed drain — use an intermediate wait without
             * the 72 ms path that kept the RX task blocking alongside CDG decode.
             */
            const TickType_t post_ticks = !s_audio_decode_enabled
                                                  ? pdMS_TO_TICKS(12)
                                          : badge_rx_is_audio_only_decode()
                                                  ? pdMS_TO_TICKS(150)
                                                  : pdMS_TO_TICKS(40);

            if (xSemaphoreTake(s_mtx, post_ticks) == pdTRUE) {
                badge_rx_deferred_audio_drain_if_pending_locked(post_burst_ms);
                if (s_video_decode_enabled != 0U) {
                    drain_cdg_to_idle(post_burst_ms);
                } else {
                    badge_rx_drain_v4_audio(post_burst_ms);
                }
                xSemaphoreGive(s_mtx);
                badge_rx_adapt_jitter_capacity_unblocked(post_burst_ms);
            } else if (badge_rx_is_audio_only_decode()) {
                s_stats.rx_mtx_postburst_drain_miss++;
            }
        }
        {
            uint64_t loop_now_ms = dashcdg_clock_now_ms();

            badge_rx_maybe_send_v4_stats(loop_now_ms);
            badge_rx_maybe_uart_log_audio_stats(loop_now_ms);
            badge_rx_maybe_periodic_igmp_refresh(loop_now_ms);
        }
    }

    ESP_LOGI(TAG, "rx task exit");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static void badge_rx_drop_multicast_membership(int fd)
{
    struct ip_mreq mr;

    if (fd < 0) {
        return;
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
    if (setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr)) != 0 && mr.imr_interface.s_addr != htonl(INADDR_ANY)) {
        mr.imr_interface.s_addr = htonl(INADDR_ANY);
        (void)setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
    }
}

/** IGMP join for BADGE_RX_MCAST_ADDR; prefers STA IPv4 when set (needed after DHCP / AP mcast quirks). */
static int badge_rx_mcast_join_ipv4(int fd)
{
    struct ip_mreq mr;

    if (fd < 0) {
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
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
        if (mr.imr_interface.s_addr != htonl(INADDR_ANY)) {
            mr.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

/**
 * DHCP renew / OpenWrt path: re-join IGMP with current STA address so L2 mcast->unicast + stack filters
 * match; also ensures ucast dup binds exist (call from IP_EVENT_STA_GOT_IP).
 */
static int badge_rx_refresh_igmp_memberships(void)
{
    int fds[3];
    size_t i;
    int joined_ok = 0;

    fds[0] = (int)s_sock;
    fds[1] = (int)s_stats_sock;
    fds[2] = (int)s_repair_sock;
    for (i = 0U; i < 3U; i++) {
        if (fds[i] < 0) {
            continue;
        }
        badge_rx_drop_multicast_membership(fds[i]);
        if (badge_rx_mcast_join_ipv4(fds[i]) != 0) {
            if (errno == ENOMEM) {
                static uint64_t s_last_igmp_enomem_warn_ms;
                const uint64_t now_ms = dashcdg_clock_now_ms();

                if (s_last_igmp_enomem_warn_ms == 0U || now_ms - s_last_igmp_enomem_warn_ms >= 5000U) {
                    s_last_igmp_enomem_warn_ms = now_ms;
                    ESP_LOGW(
                            TAG,
                            "IGMP re-join ENOMEM fd=%d internal_free=%u internal_largest=%u",
                            fds[i],
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                            (unsigned)badge_rx_internal_largest_block_bytes()
                    );
                }
            } else {
                ESP_LOGW(TAG, "IGMP re-join failed fd=%d errno=%d", fds[i], errno);
            }
        } else {
            s_stats.igmp_joined = 1U;
            joined_ok = 1;
        }
    }
    return joined_ok;
}

static int badge_rx_sta_has_ipv4(esp_netif_ip_info_t *out_ip)
{
    esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;

    if (na == NULL) {
        return 0;
    }
    if (esp_netif_get_ip_info(na, &ipi) != ESP_OK || ipi.ip.addr == 0U) {
        return 0;
    }
    if (out_ip != NULL) {
        *out_ip = ipi;
    }
    return 1;
}

static void badge_rx_maybe_periodic_igmp_refresh(uint64_t now_ms)
{
    uint32_t refresh_interval_ms;

    if (!s_run) {
        return;
    }
    if ((int)s_sock < 0 && (int)s_stats_sock < 0 && (int)s_repair_sock < 0) {
        return;
    }
    refresh_interval_ms = (s_stats.igmp_joined != 0U)
            ? BADGE_RX_IGMP_REFRESH_INTERVAL_MS
            : ((s_igmp_bootstrap_until_local_ms != 0U && now_ms < s_igmp_bootstrap_until_local_ms)
                       ? BADGE_RX_IGMP_BOOTSTRAP_RETRY_MS
                       : BADGE_RX_IGMP_REFRESH_RETRY_INTERVAL_MS);
    if (s_last_igmp_refresh_local_ms != 0U &&
        now_ms > s_last_igmp_refresh_local_ms &&
        (now_ms - s_last_igmp_refresh_local_ms) < (uint64_t)refresh_interval_ms) {
        return;
    }
    if (!badge_rx_sta_has_ipv4(NULL)) {
        return;
    }
    s_last_igmp_refresh_local_ms = now_ms;
    if (badge_rx_refresh_igmp_memberships() != 0) {
        s_last_igmp_refresh_local_ms = now_ms;
    }
    badge_rx_try_open_ucast_sockets();
}

void dashcdg_badge_rx_notify_sta_got_ip(void)
{
    if (s_rx_task == NULL) {
        /*
         * Normal path: home screen already has an IP before the user opens karaoke. We cannot
         * re-join yet (no mcast fds). Mark resync so dashcdg_badge_rx_start() + bootstrap IGMP
         * retries run after lwIP + DHCP are fully stable (avoids "no packets until reset").
         */
        s_sta_got_ip_pending_resync = 1U;
        return;
    }
#if defined(ESP_PLATFORM)
    /* DHCP renew / reconnect can restore default modem PS; IGMP reports need immediate TX. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    s_stats.igmp_joined = 0U;
    s_last_igmp_refresh_local_ms = 0U;
    (void)badge_rx_refresh_igmp_memberships();
    badge_rx_try_open_ucast_sockets();
}

static int open_multicast_rx_on_port(uint16_t port, int rcv_bytes)
{
    int s;
    struct sockaddr_in addr;
    int reuse = 1;
    int rcv = rcv_bytes;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "socket errno=%d", errno);
        return -1;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv)) != 0) {
        ESP_LOGW(TAG, "mcast port %u SO_RCVBUF=%d errno=%d", (unsigned)port, rcv, errno);
    }
    badge_rx_sock_set_dashcdg_dscp(s, "mcast_rx");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "bind errno=%d", errno);
        close(s);
        return -1;
    }

    if (badge_rx_mcast_join_ipv4(s) != 0) {
        /*
         * Keep socket bound even if IGMP join fails transiently (DHCP race / AP timing).
         * Periodic refresh and STA GOT_IP handler will retry joins while karaoke RX is active.
         */
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "IP_ADD_MEMBERSHIP errno=%d", errno);
        s_stats.igmp_joined = 0;
        ESP_LOGW(TAG, "multicast join deferred on port %u: %s", (unsigned)port, s_stats.last_error);
    } else {
        s_stats.igmp_joined = 1;
    }
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
    dashcdg_badge_rx_apply_rx_tuning_prefs();

    memset(&s_stats, 0, sizeof(s_stats));
    s_last_uart_audio_stat_ms = 0U;
    s_uart_audio_chop_warmed = 0U;
    s_uart_audio_chop_jb_peak = 0U;
    memset(&s_uart_audio_chop_prev, 0, sizeof(s_uart_audio_chop_prev));
    {
        uint8_t v = 1U;
        uint8_t a = 1U;
        (void)dashcdg_badge_prefs_load_karaoke_video_decode(&v);
        (void)dashcdg_badge_prefs_load_karaoke_audio_decode(&a);
        s_video_decode_enabled = (v != 0U) ? 1U : 0U;
        s_audio_decode_enabled = (a != 0U) ? 1U : 0U;
    }
    {
        uint8_t kov = 85U;

        (void)dashcdg_badge_prefs_load_karaoke_output_volume(&kov);
        dashcdg_platform_hw_set_karaoke_output_volume_pct(kov);
    }
    memset(s_stats.last_error, 0, sizeof(s_stats.last_error));
    s_cdg_jb_headroom_slots = BADGE_RX_JB_HEADROOM_EFFECTIVE;
    /*
     * Defer optional ucast (stats+repair STA dups) ~3 s: lets media ucast + mcast + Opus blob + DAC
     * settle before allocating two more UDP PCBs (reduces select ENOMEM + fragmentation).
     */
    s_ucast_optional_earliest_ms = dashcdg_clock_now_ms() + 3000ULL;
    if (s_audio_decode_enabled) {
#if defined(ESP_PLATFORM)
        if (!dashcdg_opus_preallocate_decoder_blob()) {
            ESP_LOGW(TAG, "Opus decoder heap reserve failed — init may hit OPUS_ALLOC_FAIL until heap frees");
        }
#endif
        (void)badge_rx_ensure_audio_jitter();
    } else {
        badge_rx_free_audio_jitter();
    }
    badge_rx_amr_decoder_reset();
    s_announced_audio_frame_ms = 0U;
    s_announced_audio_codec_id = 0U;
    s_announced_audio_profile_id = 0U;
    s_announced_audio_sample_rate = 0U;
    s_announced_audio_channels = 0U;
    s_audio_decode_primed = 0;
    badge_rx_audio_last_mono_reset();
    s_last_audio_jitter_apply_local_ms = 0U;
    memset(s_video_repair_groups, 0, sizeof(s_video_repair_groups));
    s_v4_tx_src_ipv4 = 0U;
    s_wire_seq_inited = 0U;
    s_wire_next_expected = 0U;
    s_wire_seen_bitmap = 0ULL;
    s_audio_seq_inited = 0U;
    s_audio_next_expected = 0U;
    s_cdg_seq_inited = 0U;
    s_cdg_next_expected = 0U;
    s_clock_pb_inited = 0U;
    s_clock_last_playback_ms = 0U;
    s_last_v4_stats_sent_ms = 0U;
    s_v4_stats_suppressed = 0U;
    s_last_select_enomem_local_ms = 0U;
    s_last_igmp_refresh_local_ms = 0U;
    s_igmp_bootstrap_until_local_ms = 0U;
    s_rx_mcast_media_datagrams = 0U;
    s_rx_ucast_media_datagrams = 0U;
    s_media_sequence_duplicate_hits = 0U;
    s_last_mcast_media_rx_ms = 0U;
    s_last_ucast_media_rx_ms = 0U;
    memset(s_media_dup_cache, 0, sizeof(s_media_dup_cache));
    s_media_auto_prefer_path = 0U;
    s_media_auto_hold_until_ms = 0U;
    s_media_auto_last_eval_ms = 0U;
    s_media_auto_dup_last = 0U;
    s_media_auto_mcast_last = 0U;
    s_media_auto_ucast_last = 0U;
    s_audio_rx_since_dac_push = 0U;
    s_audio_rx_since_dac_push_start_ms = 0U;
    s_audio_rx_no_dac_last_warn_ms = 0U;
    s_audio_rx_no_dac_warn_count = 0U;
    s_rx_stats_seq = 0U;
    s_v4_stats_snapshot_valid = 0U;
    memset(&s_v4_stats_snapshot, 0, sizeof(s_v4_stats_snapshot));
    s_memory_profile = BADGE_RX_MEM_PROFILE_BALANCED;
    s_memory_profile_switches = 0U;
    s_memory_profile_resize_failures = 0U;
    s_memory_profile_resize_audio_dropped = 0U;
    s_memory_profile_resize_video_dropped = 0U;
    s_memory_profile_adaptive_grows = 0U;
    s_memory_profile_adaptive_shrinks = 0U;
    s_last_adapt_tune_ms = 0U;
    s_last_adapt_jb_evict_rounds = 0U;
    s_last_adapt_fast_mode = 0U;
    s_last_adapt_audio_max_slots = (uint16_t)BADGE_RX_AUDIO_SLOTS_MAX;
    s_last_adapt_video_max_slots = (uint16_t)BADGE_RX_VIDEO_SLOTS_MAX;
    s_last_adapt_grow_ms = 0U;
    s_last_adapt_resize_fail_ms = 0U;
    s_adapt_resize_fail_streak = 0U;
    s_last_adapt_budget_bytes = 0U;
    s_last_adapt_budget_contig_bytes = 0U;
    s_last_adapt_budget_audio_bytes = 0U;
    s_last_adapt_budget_video_bytes = 0U;
    s_last_adapt_burst_ms = 0U;
    s_last_presented_audio_timestamp_ms = 0U;
    badge_rx_ensure_receiver_instance_id();

    dashcdg_media_clock_init(&s_mclk);
    s_jitter_cdg_primed = 0;
    s_last_cdg_apply_local_ms = 0U;
    s_cdg_skip_hold_until_local_ms = 0U;
    s_announced_playout_delay_ms = 0U;
    s_sync_local_ms = 0U;
    s_sync_playback_ms = 0U;
    s_active_session_start_ms = 0U;
    s_active_session_local_start_ms = 0U;
    s_active_asset_size = 0U;
    memset(s_active_song_id, 0, sizeof(s_active_song_id));
    s_active_session_valid = 0;
    s_cdg_skip_hold_until_local_ms = 0U;

    s_cdg_blit_max_y = DASHCDG_BADGE_RX_VISIBLE_H;
    if (s_video_decode_enabled) {
        if (badge_rx_ensure_heap() != 0) {
            ESP_LOGW(TAG, "CDG/jitter unavailable — multicast RX, parse, clock sync still run");
        } else {
            dashcdg_cdg_state_init(s_cdg);
            dashcdg_cdg_batch_jitter_init(s_jb);
            dashcdg_cdg_state_raster_dirty_mark_full(s_cdg);
            if (badge_rx_ensure_blit_scratch() != 0) {
                ESP_LOGW(TAG, "CDG blit scratch OOM (~7 KiB) — decode+jitter ok, LCD band blits off until heap frees");
            } else {
#ifdef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
                ESP_LOGI(TAG, "CDG+jitter heap + blit scratch; v4 anchors decode into CDG");
#else
                ESP_LOGI(TAG, "CDG+jitter static + blit scratch; v4 anchors decode into CDG (no extra decode heap)");
#endif
            }
        }
    } else {
        s_cdg = NULL;
        s_jb = NULL;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) == pdTRUE) {
        badge_rx_apply_memory_profile_locked();
        xSemaphoreGive(s_mtx);
    }

    /*
     * Modem PS defers TX (including IGMP reports). Karaoke UI sets PS_NONE via set_screen before
     * rx_start; this covers panel-wake / other callers so the first IP_ADD_MEMBERSHIP isn't wasted.
     */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    s_sock = open_multicast_rx_on_port((uint16_t)BADGE_RX_PORT, BADGE_RX_RCVBUF_MEDIA);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "open_multicast_rx failed: %s", s_stats.last_error);
        badge_rx_free_heap();
        return;
    }

    s_stats_sock = open_multicast_rx_on_port((uint16_t)BADGE_RX_TX_STATS_PORT, BADGE_RX_RCVBUF_CONTROL);
    if (s_stats_sock < 0) {
        ESP_LOGW(TAG, "stats multicast socket open failed: %s", s_stats.last_error);
    }

    s_uplink_sock = badge_rx_open_uplink_socket();
    if (s_uplink_sock < 0) {
        ESP_LOGW(TAG, "v4 control uplink socket failed (stats/NACK share stats recv socket if open): %s", s_stats.last_error);
    }

    s_repair_sock = open_multicast_rx_on_port((uint16_t)BADGE_RX_REPAIR_PORT, BADGE_RX_RCVBUF_CONTROL);
    if (s_repair_sock < 0) {
        ESP_LOGW(TAG, "repair multicast socket open failed (FEC repair on split port will be missing): %s",
                 s_stats.last_error);
    }

    if (badge_rx_sta_has_ipv4(NULL)) {
        /* Joins in open_multicast_* may have used INADDR_ANY before DHCP; align IGMP + STA ucast for AP mcast->ucast. */
        s_stats.igmp_joined = 0U;
        s_last_igmp_refresh_local_ms = 0U;
        (void)badge_rx_refresh_igmp_memberships();
    }
    badge_rx_try_open_ucast_sockets();

    s_run = 1;
#if CONFIG_FREERTOS_UNICORE
    if (xTaskCreate(badge_rx_task, "badge_rx", BADGE_RX_STACK, NULL, BADGE_RX_TASK_PRIO, &s_rx_task) != pdPASS) {
#else
    if (xTaskCreatePinnedToCore(badge_rx_task, "badge_rx", BADGE_RX_STACK, NULL, BADGE_RX_TASK_PRIO, &s_rx_task,
                                BADGE_RX_TASK_CORE) != pdPASS) {
#endif
        s_run = 0;
        badge_rx_close_ucast_sockets();
        close(s_sock);
        s_sock = -1;
        if (s_stats_sock >= 0) {
            close(s_stats_sock);
            s_stats_sock = -1;
        }
        if (s_repair_sock >= 0) {
            close(s_repair_sock);
            s_repair_sock = -1;
        }
        if (s_uplink_sock >= 0) {
            close(s_uplink_sock);
            s_uplink_sock = -1;
        }
        snprintf(s_stats.last_error, sizeof(s_stats.last_error), "xTaskCreate failed");
        ESP_LOGE(TAG, "%s", s_stats.last_error);
        badge_rx_free_heap();
    } else {
        {
            uint8_t um = badge_rx_ucast_mask_from_fds();

            ESP_LOGI(TAG, "listening UDP %s:%d media, repair:%d, stats:%d (ucast mask 0x%02x)", BADGE_RX_MCAST_ADDR,
                     BADGE_RX_PORT, BADGE_RX_REPAIR_PORT, BADGE_RX_TX_STATS_PORT, (unsigned)um);
        }
        if (s_audio_decode_enabled != 0U) {
            dashcdg_platform_hw_karaoke_amp_arm_for_rx();
        }
        /*
         * First join after cold boot can race DHCP/lwIP; OpenWrt mcast→ucast also needs ucast dup
         * binds on the STA address. Yield, re-open dups, drop+re-join with the real interface, and
         * (most sessions) one more pass after IP was seen before RX existed.
         */
        s_igmp_bootstrap_until_local_ms = dashcdg_clock_now_ms() + (uint64_t)BADGE_RX_IGMP_BOOTSTRAP_WINDOW_MS;
        vTaskDelay(pdMS_TO_TICKS(BADGE_RX_IGMP_POST_START_DELAY_MS));
        s_stats.igmp_joined = 0U;
        s_last_igmp_refresh_local_ms = 0U;
        (void)badge_rx_refresh_igmp_memberships();
        badge_rx_try_open_ucast_sockets();
        if (s_sta_got_ip_pending_resync != 0U) {
            s_sta_got_ip_pending_resync = 0U;
            vTaskDelay(pdMS_TO_TICKS(BADGE_RX_IGMP_POST_STAIP_EXTRA_DELAY_MS));
            s_stats.igmp_joined = 0U;
            (void)badge_rx_refresh_igmp_memberships();
            badge_rx_try_open_ucast_sockets();
            ESP_LOGI(TAG, "IGMP bootstrap: STA got IP before karaoke RX started — extra join pass done");
        }
    }
}

void dashcdg_badge_rx_stop(void)
{
    int fd;

    s_stats.igmp_joined = 0;
    s_run = 0;
    s_v4_tx_src_ipv4 = 0U;
    s_last_v4_stats_sent_ms = 0U;
    s_last_igmp_refresh_local_ms = 0U;
    s_igmp_bootstrap_until_local_ms = 0U;
    badge_rx_close_ucast_sockets();
    fd = (int)s_sock;
    if (fd >= 0) {
        s_sock = -1;
        badge_rx_drop_multicast_membership(fd);
        close(fd);
    }
    fd = (int)s_stats_sock;
    if (fd >= 0) {
        s_stats_sock = -1;
        badge_rx_drop_multicast_membership(fd);
        close(fd);
    }
    fd = (int)s_uplink_sock;
    if (fd >= 0) {
        s_uplink_sock = -1;
        close(fd);
    }
    fd = (int)s_repair_sock;
    if (fd >= 0) {
        s_repair_sock = -1;
        badge_rx_drop_multicast_membership(fd);
        close(fd);
    }
    for (int i = 0; i < 80 && s_rx_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    badge_rx_v4_anchor_asm_reset();
    dashcdg_platform_hw_karaoke_dac_stop();
    badge_rx_amr_decoder_reset();
    badge_rx_free_audio_jitter();
    s_audio_decode_primed = 0;
    badge_rx_audio_last_mono_reset();
    s_last_audio_jitter_apply_local_ms = 0U;
    s_last_presented_audio_timestamp_ms = 0U;
#ifdef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
    badge_rx_free_heap();
#else
    if (s_jb) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
    }
    /* Static CDG/jitter: keep wired across karaoke exits; scratch stays allocated for fast re-entry. */
#endif
    ESP_LOGI(TAG, "rx stopped");
}

bool dashcdg_badge_rx_is_running(void)
{
    return s_rx_task != NULL;
}

void dashcdg_badge_rx_set_decode_enabled(bool video_on, bool audio_on)
{
    uint8_t audio_was_enabled;

    if (s_mtx == NULL) {
        s_video_decode_enabled = video_on ? 1U : 0U;
        s_audio_decode_enabled = audio_on ? 1U : 0U;
        if (audio_on) {
            dashcdg_platform_hw_karaoke_amp_arm_for_rx();
        }
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }
    audio_was_enabled = s_audio_decode_enabled;
    s_video_decode_enabled = video_on ? 1U : 0U;
    s_audio_decode_enabled = audio_on ? 1U : 0U;
    if (!s_video_decode_enabled && s_jb != NULL) {
        dashcdg_cdg_batch_jitter_clear(s_jb);
        s_jitter_cdg_primed = 0;
    }
    if (!s_audio_decode_enabled) {
        if (s_audio_jb != NULL) {
            dashcdg_audio_jitter_clear(s_audio_jb);
        }
        badge_rx_amr_decoder_reset();
        s_audio_decode_primed = 0;
        badge_rx_audio_last_mono_reset();
        s_last_audio_jitter_apply_local_ms = 0U;
        s_last_presented_audio_timestamp_ms = 0U;
    } else if (s_audio_decode_enabled && !audio_was_enabled) {
        (void)badge_rx_ensure_audio_jitter();
        s_audio_decode_primed = 0U;
        s_last_audio_jitter_apply_local_ms = 0U;
        s_last_presented_audio_timestamp_ms = 0U;
    }
    badge_rx_apply_memory_profile_locked();
    xSemaphoreGive(s_mtx);
    if (audio_on) {
        dashcdg_platform_hw_karaoke_amp_arm_for_rx();
    } else {
        dashcdg_platform_hw_karaoke_dac_stop();
    }
}

void dashcdg_badge_rx_get_decode_enabled(bool *video_on, bool *audio_on)
{
    if (video_on == NULL || audio_on == NULL) {
        return;
    }
    if (s_mtx == NULL) {
        *video_on = (s_video_decode_enabled != 0U);
        *audio_on = (s_audio_decode_enabled != 0U);
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
        return;
    }
    *video_on = (s_video_decode_enabled != 0U);
    *audio_on = (s_audio_decode_enabled != 0U);
    xSemaphoreGive(s_mtx);
}

void dashcdg_badge_rx_apply_rx_tuning_prefs(void)
{
    uint8_t n = 1U;
    uint8_t stx = 1U;
    uint8_t mp = BADGE_RX_MEDIA_PATH_POLICY_AUTO;

    (void)dashcdg_badge_prefs_load_karaoke_repair_nack(&n);
    (void)dashcdg_badge_prefs_load_karaoke_v4_stats_tx(&stx);
    (void)dashcdg_badge_prefs_load_karaoke_media_path_policy(&mp);
#if !CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
    n = 0U;
#endif
    if (mp > BADGE_RX_MEDIA_PATH_POLICY_AUTO) {
        mp = BADGE_RX_MEDIA_PATH_POLICY_AUTO;
    }
    if (s_mtx != NULL && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) == pdTRUE) {
        s_repair_nack_enabled = (n != 0U) ? 1U : 0U;
        s_v4_stats_tx_enabled = (stx != 0U) ? 1U : 0U;
        s_media_path_policy = mp;
        xSemaphoreGive(s_mtx);
    } else {
        s_repair_nack_enabled = (n != 0U) ? 1U : 0U;
        s_v4_stats_tx_enabled = (stx != 0U) ? 1U : 0U;
        s_media_path_policy = mp;
    }
}

/** Fields that do not require `s_mtx` (or are intentionally re-read) after a mutex snapshot. */
static void badge_rx_stats_finalize_public_view(dashcdg_badge_rx_stats_t *out, uint8_t snap_vdec, uint8_t snap_adec)
{
    out->v4_repair_rx_socket_ok = (s_repair_sock >= 0) ? 1U : 0U;
    out->v4_control_uplink_ok = (s_uplink_sock >= 0) ? 1U : 0U;
    out->ucast_rx_mask = badge_rx_ucast_mask_from_fds();
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
    /* audio_slots_capacity / video_slots_capacity: snapshotted under s_mtx with pending counts
     * in dashcdg_badge_rx_get_stats — do not re-read jitter here (race skewed HUD a%/v%). */
    out->memory_profile = (uint8_t)s_memory_profile;
    out->memory_profile_switches = s_memory_profile_switches;
    out->memory_profile_resize_failures = s_memory_profile_resize_failures;
    out->memory_profile_resize_audio_dropped = s_memory_profile_resize_audio_dropped;
    out->memory_profile_resize_video_dropped = s_memory_profile_resize_video_dropped;
    out->memory_profile_adaptive_grows = s_memory_profile_adaptive_grows;
    out->memory_profile_adaptive_shrinks = s_memory_profile_adaptive_shrinks;
    out->mcast_media_datagrams = s_rx_mcast_media_datagrams;
    out->ucast_media_datagrams = s_rx_ucast_media_datagrams;
    out->media_sequence_duplicate_hits = s_media_sequence_duplicate_hits;
    out->audio_rx_no_dac_warn = s_audio_rx_no_dac_warn_count;
    out->v4_rx_stats_suppressed = s_v4_stats_suppressed;
    out->media_path_policy = s_media_path_policy;
    out->video_decode_enabled = (snap_vdec != 0U) ? 1U : 0U;
    out->audio_decode_enabled = (snap_adec != 0U) ? 1U : 0U;
    snprintf(out->tx_stats_dest, sizeof(out->tx_stats_dest), "--");
    if (s_v4_tx_src_ipv4 != 0U) {
        uint32_t h = ntohl(s_v4_tx_src_ipv4);
        snprintf(out->tx_stats_dest, sizeof(out->tx_stats_dest), "%u.%u.%u.%u", (unsigned)((h >> 24) & 0xffU),
                 (unsigned)((h >> 16) & 0xffU), (unsigned)((h >> 8) & 0xffU), (unsigned)(h & 0xffU));
    }
}

void dashcdg_badge_rx_get_stats(dashcdg_badge_rx_stats_t *out)
{
    uint8_t snap_vdec = 0U;
    uint8_t snap_adec = 0U;

    if (out == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_jb) {
            s_stats.jb_next_packet_index = s_jb->next_packet_index;
            s_stats.jb_pending_slots = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
            s_stats.video_slots_capacity = (uint16_t)dashcdg_cdg_batch_jitter_capacity(s_jb);
        } else {
            s_stats.jb_next_packet_index = 0;
            s_stats.jb_pending_slots = 0;
            s_stats.video_slots_capacity = 0U;
        }
        if (s_audio_jb != NULL) {
            s_stats.audio_jb_pending_slots = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
            s_stats.audio_slots_capacity = (uint16_t)dashcdg_audio_jitter_capacity(s_audio_jb);
        } else {
            s_stats.audio_jb_pending_slots = 0U;
            s_stats.audio_slots_capacity = 0U;
        }
        s_stats.repair_nack_enabled = s_repair_nack_enabled;
        s_stats.v4_stats_tx_enabled = s_v4_stats_tx_enabled;
        s_stats.v4_rx_stats_suppressed = s_v4_stats_suppressed;
        s_stats.v4_repair_rx_socket_ok = (s_repair_sock >= 0) ? 1U : 0U;
        s_stats.v4_control_uplink_ok = (s_uplink_sock >= 0) ? 1U : 0U;
        s_stats.ucast_rx_mask = badge_rx_ucast_mask_from_fds();
        s_stats.mcast_media_datagrams = s_rx_mcast_media_datagrams;
        s_stats.ucast_media_datagrams = s_rx_ucast_media_datagrams;
        s_stats.media_sequence_duplicate_hits = s_media_sequence_duplicate_hits;
        s_stats.audio_rx_no_dac_warn = s_audio_rx_no_dac_warn_count;
        s_stats.media_path_policy = s_media_path_policy;
        snap_vdec = s_video_decode_enabled;
        snap_adec = s_audio_decode_enabled;
        *out = s_stats;
        xSemaphoreGive(s_mtx);
    } else {
        *out = s_stats;
        snap_vdec = s_video_decode_enabled;
        snap_adec = s_audio_decode_enabled;
    }
    badge_rx_stats_finalize_public_view(out, snap_vdec, snap_adec);
}

void dashcdg_badge_rx_lvgl_overlay_tick_and_get_stats(lv_obj_t *cdg_lv_slot, dashcdg_badge_rx_stats_t *out)
{
    uint8_t snap_vdec = 0U;
    uint8_t snap_adec = 0U;

    if (out == NULL) {
        return;
    }
#if defined(CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT) && CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT
    if (cdg_lv_slot != NULL && lv_obj_is_valid(cdg_lv_slot) && dashcdg_display_lcd_panel()) {
        dashcdg_sp_blit_worker_try_init();
    }
#endif
    /*
     * Short timeout: stale HUD stats are fine; blocking LVGL 50 ms per tick made the whole UI feel
     * frozen (especially with audio-only + RX contention).
     */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        dashcdg_badge_rx_get_stats(out);
        return;
    }
    if (s_jb) {
        s_stats.jb_next_packet_index = s_jb->next_packet_index;
        s_stats.jb_pending_slots = dashcdg_cdg_batch_jitter_occupied_count(s_jb);
        s_stats.video_slots_capacity = (uint16_t)dashcdg_cdg_batch_jitter_capacity(s_jb);
    } else {
        s_stats.jb_next_packet_index = 0;
        s_stats.jb_pending_slots = 0;
        s_stats.video_slots_capacity = 0U;
    }
    if (s_audio_jb != NULL) {
        s_stats.audio_jb_pending_slots = (uint32_t)dashcdg_audio_jitter_occupied_count(s_audio_jb);
        s_stats.audio_slots_capacity = (uint16_t)dashcdg_audio_jitter_capacity(s_audio_jb);
    } else {
        s_stats.audio_jb_pending_slots = 0U;
        s_stats.audio_slots_capacity = 0U;
    }
    s_stats.repair_nack_enabled = s_repair_nack_enabled;
    s_stats.v4_stats_tx_enabled = s_v4_stats_tx_enabled;
    s_stats.v4_rx_stats_suppressed = s_v4_stats_suppressed;
    s_stats.v4_repair_rx_socket_ok = (s_repair_sock >= 0) ? 1U : 0U;
    s_stats.v4_control_uplink_ok = (s_uplink_sock >= 0) ? 1U : 0U;
    s_stats.ucast_rx_mask = badge_rx_ucast_mask_from_fds();
    s_stats.mcast_media_datagrams = s_rx_mcast_media_datagrams;
    s_stats.ucast_media_datagrams = s_rx_ucast_media_datagrams;
    s_stats.media_sequence_duplicate_hits = s_media_sequence_duplicate_hits;
    s_stats.audio_rx_no_dac_warn = s_audio_rx_no_dac_warn_count;
    s_stats.media_path_policy = s_media_path_policy;
    snap_vdec = s_video_decode_enabled;
    snap_adec = s_audio_decode_enabled;
    *out = s_stats;
    xSemaphoreGive(s_mtx);
    badge_rx_stats_finalize_public_view(out, snap_vdec, snap_adec);
    /*
     * CDG blit must not run under the same `s_mtx` hold as stats refresh: deferred enqueue would
     * block the RX task for tens of ms (pool wait × bands) and breaks A+V (black video + dead audio).
     * Re-enter via `dashcdg_badge_rx_cdg_overlay_tick` (short mutex scope around raster + queue only).
     * Audio-only: skip entirely — avoids a second `s_mtx` wait every LVGL tick (was freezing UI).
     */
    if (snap_vdec != 0U) {
        dashcdg_badge_rx_cdg_overlay_tick(cdg_lv_slot);
    }
}

void dashcdg_badge_rx_format_mcast_modal(char *buf, size_t buf_sz)
{
    dashcdg_badge_rx_stats_t st;
    size_t used = 0U;
    size_t cdg_slots_cap;
    size_t audio_slots_cap;
    unsigned long cdg_jb_bytes;
    unsigned long audio_jb_bytes;
    unsigned long blit_bytes = (unsigned long)BADGE_RX_BLIT_SCRATCH_BYTES;
    unsigned long anchor_max_bytes = (unsigned long)BADGE_RX_V4_ANCHOR_ENCODED_MAX_BYTES;
    unsigned long heap_free = (unsigned long)esp_get_free_heap_size();
    unsigned long heap_min = (unsigned long)esp_get_minimum_free_heap_size();
    unsigned long int_total = (unsigned long)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned long int_free = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned long int_largest = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned long dma_total = (unsigned long)heap_caps_get_total_size(MALLOC_CAP_DMA);
    unsigned long dma_free = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA);
    unsigned long dma_largest = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    unsigned long int_used = (int_total > int_free) ? (int_total - int_free) : 0UL;
    unsigned long dma_used = (dma_total > dma_free) ? (dma_total - dma_free) : 0UL;
    unsigned long static_bss_known = 0UL;
    unsigned long runtime_audio_alloc = 0UL;
    unsigned long runtime_cdg_alloc = 0UL;
    unsigned long runtime_blit_alloc = 0UL;
    unsigned long runtime_anchor_alloc = 0UL;
    unsigned audio_occ_pct = 0U;
    unsigned video_occ_pct = 0U;
    const char *profile_name = "balanced";

    if (buf == NULL || buf_sz < 32) {
        return;
    }
    buf[0] = '\0';
    dashcdg_badge_rx_get_stats(&st);
    cdg_slots_cap = (size_t)st.video_slots_capacity;
    audio_slots_cap = (size_t)st.audio_slots_capacity;
    if (cdg_slots_cap == 0U) {
        cdg_slots_cap = (size_t)DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT;
    }
    if (audio_slots_cap == 0U) {
        audio_slots_cap = (size_t)DASHCDG_AUDIO_JITTER_SLOT_COUNT;
    }
    cdg_jb_bytes = (unsigned long)(cdg_slots_cap * sizeof(struct dashcdg_cdg_batch_jitter_frame));
    audio_jb_bytes = (unsigned long)(audio_slots_cap * sizeof(struct dashcdg_audio_jitter_frame));
    switch (st.memory_profile) {
    case BADGE_RX_MEM_PROFILE_MINIMAL:
        profile_name = "minimal";
        break;
    case BADGE_RX_MEM_PROFILE_AUDIO_PRIORITY:
        profile_name = "audio";
        break;
    case BADGE_RX_MEM_PROFILE_VIDEO_PRIORITY:
        profile_name = "video";
        break;
    default:
        profile_name = "balanced";
        break;
    }
    if (audio_slots_cap > 0U) {
        audio_occ_pct = (unsigned)(((size_t)st.audio_jb_pending_slots * 100U) / audio_slots_cap);
    }
    if (cdg_slots_cap > 0U) {
        video_occ_pct = (unsigned)(((size_t)st.jb_pending_slots * 100U) / cdg_slots_cap);
    }
    cdg_jb_bytes = (unsigned long)(cdg_slots_cap * sizeof(struct dashcdg_cdg_batch_jitter_frame));
    audio_jb_bytes = (unsigned long)(audio_slots_cap * sizeof(struct dashcdg_audio_jitter_frame));
#ifndef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
    static_bss_known += (unsigned long)sizeof(s_cdg_storage);
    static_bss_known += (unsigned long)sizeof(s_jb_storage);
#endif
    static_bss_known += (unsigned long)sizeof(s_v4_anchor_chunk_seen);
    static_bss_known += (unsigned long)sizeof(s_video_repair_groups);
    if (s_audio_jb != NULL) {
        runtime_audio_alloc += audio_jb_bytes;
    }
    if (s_amr_pcm48_scratch != NULL) {
        runtime_audio_alloc += (unsigned long)(s_pcm_scratch_samples * sizeof(int16_t));
    }
    if (s_cdg != NULL) {
        runtime_cdg_alloc += (unsigned long)sizeof(struct dashcdg_cdg_state);
    }
    if (s_jb != NULL) {
        runtime_cdg_alloc += cdg_jb_bytes;
    }
    if (s_cdg_blit_scratch != NULL) {
        runtime_blit_alloc += blit_bytes;
    }
    if (s_v4_anchor_asm_buf != NULL) {
        runtime_anchor_alloc += (unsigned long)s_v4_anchor_asm_total_bytes;
    }
    used += (size_t)snprintf(
        buf + used, (used < buf_sz) ? (buf_sz - used) : 0U,
        "--- Multicast / RX ---\n"
        "group ... %s:%d\n"
        "STA ip . %s\n"
        "IGMP ... %s\n"
        "task ... %s\n"
        "CDG decode %s\n"
        "stats->TX %s:%d (sent %lu)\n"
        "stats suppressed %lu  audio_no_dac_warn %lu\n"
        "path policy %u  media m/u %lu/%lu  dup_seq %lu\n"
        "raster Y<=%u  jb_evict %lu  ins fl %lu dup %lu st %lu\n"
        "\n"
        "datagrams %llu\n"
        "parse_fail %lu\n"
        "v4 session %lu  clock %lu\n"
        "audio rx %lu  out %lu  dec_fail %lu  dac_begin_fail %lu  unsup %lu\n"
        "audio codec switches %lu  codec mismatch %lu\n"
        "delta %lu  anchor %lu  anch_rej %lu  load %lu\n"
        "rwin %lu  fwd %lu  rev %lu\n"
        "nack ok %lu att %lu fail %lu thr %lu\n"
        "rrec %lu  rfail %lu\n"
        "wire miss_est %llu  reorder %lu\n"
        "miss by type a/c/clk/rep %lu/%lu/%lu/%lu\n"
        "seq %llu  skew_ema %ld ms\n"
        "have_clock %d\n"
        "song_id %s\n"
        "jb next %llu pend %u skips %llu\n"
        "\n"
        "alloc cdg_jb %luB (%u slots)  audio_jb %luB (%u slots)\n"
        "alloc blit %luB  anchor_max %luB\n"
        "profile %s  switches %lu  resize_fail %lu\n"
        "resize_drop audio %lu  video %lu  adaptive_grow %lu  shrink %lu\n"
        "adaptive mode %s  cap audio<=%u video<=%u\n"
        "adapt budget dyn %luB  contig %luB  split a/v %luB/%luB\n"
        "occupancy audio %u/%u (%u%%)  video %u/%u (%u%%)\n"
        "runtime cdg_heap %s  audio_jb %s  blit %s\n"
        "runtime bytes cdg %luB  audio %luB  blit %luB  anchor %luB\n"
        "\n"
        "sys heap free %luB  min %luB\n"
        "int mem  used %luB/%luB  free %luB  largest %luB\n"
        "dma mem  used %luB/%luB  free %luB  largest %luB\n"
        "known static bss %luB",
        BADGE_RX_MCAST_ADDR, BADGE_RX_PORT, st.sta_ip[0] ? st.sta_ip : "--", st.igmp_joined ? "joined" : "not joined",
        st.rx_task_running ? "running" : "stopped",
        st.cdg_heap_ok
#ifdef CONFIG_DASHCDG_BADGE_RX_CDG_ON_HEAP
            ? "on (heap CDG+jitter+scratch)"
#else
            ? "on (bss CDG+jitter + scratch)"
#endif
            : "off",
        (st.tx_stats_dest[0] && st.tx_stats_dest[0] != '-') ? st.tx_stats_dest : "(wire src?)", BADGE_RX_TX_STATS_PORT,
        (unsigned long)st.v4_rx_stats_sent,
        (unsigned long)st.v4_rx_stats_suppressed, (unsigned long)st.audio_rx_no_dac_warn,
        (unsigned)st.media_path_policy, (unsigned long)st.mcast_media_datagrams, (unsigned long)st.ucast_media_datagrams,
        (unsigned long)st.media_sequence_duplicate_hits, (unsigned)st.cdg_blit_max_y, (unsigned long)st.jb_evict_rounds,
        (unsigned long)st.cdg_delta_insert_fail, (unsigned long)st.cdg_delta_insert_dup, (unsigned long)st.cdg_delta_insert_stale,
        (unsigned long long)st.datagrams, (unsigned long)st.parse_failures,
        (unsigned long)st.v4_session_count, (unsigned long)st.v4_clock_count, (unsigned long)st.v4_audio_chunk_rx,
        (unsigned long)st.v4_audio_frames_out, (unsigned long)st.v4_audio_decode_fail,
        (unsigned long)st.v4_audio_dac_begin_fail, (unsigned long)st.v4_audio_unsupported_codec,
        (unsigned long)st.v4_audio_codec_switches, (unsigned long)st.v4_audio_codec_mismatch,
        (unsigned long)st.v4_video_delta_count, (unsigned long)st.v4_anchor_chunks, (unsigned long)st.v4_anchor_rejected_behind,
        (unsigned long)st.v4_loading_screen_count,
        (unsigned long)st.v4_video_repair_rx_packets, (unsigned long)st.v4_video_repair_rx_forward,
        (unsigned long)st.v4_video_repair_rx_reverse, (unsigned long)st.v4_repair_nack_tx,
        (unsigned long)st.v4_repair_nack_attempt, (unsigned long)st.v4_repair_nack_send_fail,
        (unsigned long)st.v4_repair_nack_throttled,
        (unsigned long)st.v4_video_repair_recovered,
        (unsigned long)st.v4_video_repair_failed, (unsigned long long)st.wire_missing_estimate,
        (unsigned long)st.wire_reorder_events, (unsigned long)st.audio_missing_estimate, (unsigned long)st.cdg_missing_estimate,
        (unsigned long)st.clock_missing_estimate, (unsigned long)st.repair_missing_estimate,
        (unsigned long long)st.last_sequence, (long)st.skew_ema_ms, st.have_clock,
        st.song_id[0] ? st.song_id : "(none)", (unsigned long long)st.jb_next_packet_index, (unsigned)st.jb_pending_slots,
        (unsigned long long)st.live_missing_skips, cdg_jb_bytes, (unsigned)cdg_slots_cap, audio_jb_bytes, (unsigned)audio_slots_cap,
        blit_bytes, anchor_max_bytes, profile_name, (unsigned long)st.memory_profile_switches,
        (unsigned long)st.memory_profile_resize_failures, (unsigned long)st.memory_profile_resize_audio_dropped,
        (unsigned long)st.memory_profile_resize_video_dropped, (unsigned long)st.memory_profile_adaptive_grows,
        (unsigned long)st.memory_profile_adaptive_shrinks,
        s_last_adapt_fast_mode ? "fast-pressure" : "normal", (unsigned)s_last_adapt_audio_max_slots,
        (unsigned)s_last_adapt_video_max_slots, (unsigned long)s_last_adapt_budget_bytes,
        (unsigned long)s_last_adapt_budget_contig_bytes, (unsigned long)s_last_adapt_budget_audio_bytes,
        (unsigned long)s_last_adapt_budget_video_bytes, (unsigned)st.audio_jb_pending_slots, (unsigned)audio_slots_cap, audio_occ_pct,
        (unsigned)st.jb_pending_slots, (unsigned)cdg_slots_cap, video_occ_pct,
        (s_cdg != NULL && s_jb != NULL) ? "on" : "off", (s_audio_jb != NULL) ? "on" : "off",
        (s_cdg_blit_scratch != NULL) ? "on" : "off", runtime_cdg_alloc, runtime_audio_alloc, runtime_blit_alloc,
        runtime_anchor_alloc, heap_free, heap_min, int_used, int_total, int_free, int_largest,
        dma_used, dma_total, dma_free, dma_largest, static_bss_known);

    if (st.last_error[0]) {
        used += (size_t)snprintf(buf + used, (used < buf_sz) ? (buf_sz - used) : 0U, "\n\nlast_error: %s", st.last_error);
    }
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
#if defined(CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT) && CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT
        {
#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
            const int64_t t_rgb0 = esp_timer_get_time();
#endif
            for (int y = vy0; y < vy1;) {
                int bh = vy1 - y;
                if (bh > DASHCDG_BADGE_RX_BLIT_BAND_H) {
                    bh = DASHCDG_BADGE_RX_BLIT_BAND_H;
                }
                {
                    int vw = vx1 - vx0;

                    dashcdg_badge_cdg_state_rect_to_rgb565_le(s_cdg, vx0, y, vw, bh, s_cdg_blit_scratch, (size_t)vw);
                    /*
                     * Non-blocking slot wait: blocking here runs under `s_mtx` (LVGL + RX). Even a few
                     * ticks per band starves multicast pump + audio drain when video is on (choppy Opus / silence).
                     */
                    if (dashcdg_sp_blit_enqueue_band(ox + vx0, oy + y, vw, bh, s_cdg_blit_scratch, 0) != ESP_OK) {
                        s_stats.cdg_blit_band_drop++;
                    }
                }
                y += bh;
            }
#if defined(CONFIG_DASHCDG_BADGE_PERF_PROBES) && CONFIG_DASHCDG_BADGE_PERF_PROBES
            {
                int64_t dt_rgb = esp_timer_get_time() - t_rgb0;

                if (dt_rgb > 0 && dt_rgb < 5000000) {
                    badge_rx_perf_note_mtx_overlay_rgb_us((uint32_t)dt_rgb);
                }
            }
#endif
        }
#else
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
                esp_rom_delay_us((uint32_t)DASHCDG_BADGE_RX_PANEL_BAND_SETTLE_US);
            }
            y += bh;
        }
#endif
    }
}

void dashcdg_badge_rx_cdg_overlay_tick(lv_obj_t *cdg_lv_slot)
{
    if (cdg_lv_slot == NULL || !lv_obj_is_valid(cdg_lv_slot) || !dashcdg_display_lcd_panel()) {
        return;
    }

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(16)) != pdTRUE) {
        return;
    }
    if (s_cdg == NULL) {
        xSemaphoreGive(s_mtx);
        return;
    }
    if (!s_video_decode_enabled) {
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

        dashcdg_badge_rx_lvgl_overlay_tick_and_get_stats(cdg_lv_slot, &st);
        snprintf(line, sizeof(line), "dg %llu | seq %llu | (i) mcast", (unsigned long long)st.datagrams,
                 (unsigned long long)st.last_sequence);
        lv_label_set_text(lbl, line);
    } else {
        dashcdg_badge_rx_cdg_overlay_tick(cdg_lv_slot);
    }
}
