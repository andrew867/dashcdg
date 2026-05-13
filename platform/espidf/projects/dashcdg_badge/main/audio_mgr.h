#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Audio manager owner task (T11).
 *
 * Purpose: ensure the RX hot path never blocks inside DAC begin/write calls by moving
 * all `dashcdg_platform_hw_karaoke_dac_*` calls onto a single lower-scope owner task.
 */

esp_err_t dashcdg_audio_mgr_init(void);

/**
 * Enqueue mono PCM samples for playback at nominal sample rate `nom_hz`.
 *
 * Non-blocking: drops when the internal ring is full. The drop is counted so soak logs can
 * attribute glitches to CPU/queue pressure rather than "codec jitter".
 */
bool dashcdg_audio_mgr_push_mono_s16(uint32_t nom_hz, const int16_t *mono, size_t mono_samples);

/** Flushes/pads partial chunks and keeps the DAC handle open (track boundary). */
void dashcdg_audio_mgr_session_break(void);

/** Stops the DAC output path (karaoke exit). */
void dashcdg_audio_mgr_stop(void);

/** Apply a drift trim hint (PPM) to the DAC effective sample rate. */
void dashcdg_audio_mgr_set_trim_ppm(int32_t ppm);

typedef struct {
    uint32_t pcm_drop_full;
    uint32_t pcm_drop_oldest;
    uint32_t underrun_silence_frames;
    uint32_t dac_begin_fail;
    uint32_t frames_pushed;
    uint32_t bytes_pushed;
    uint32_t last_nom_hz;
    uint32_t queued_samples;
} dashcdg_audio_mgr_stats_t;

void dashcdg_audio_mgr_get_stats(dashcdg_audio_mgr_stats_t *out);
