#ifndef DASHCDG_PCM_RATE_CONVERT_H
#define DASHCDG_PCM_RATE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/*
 * High-quality rate conversion for desktop: exact-ratio polyphase FIR (48↔8/16 kHz),
 * zero-stuff + FIR for integer 8/16 kHz → 48 kHz, and Lanczos band-limited resampling
 * for other ratios (e.g. 44.1 kHz → 48 kHz). Public name is historical (`_cubic`);
 * Catmull–Rom cubics are no longer used in the default path.
 */
void dashcdg_pcm_mono_resample_cubic(
        const int16_t *in,
        size_t in_len,
        uint32_t in_rate,
        int16_t *out,
        size_t out_len,
        uint32_t out_rate
);

void dashcdg_pcm_stereo_interleaved_to_mono48(
        const int16_t *pcm48_interleaved,
        size_t frame_count,
        int16_t *mono48_out
);

void dashcdg_pcm_interleaved_to_mono(
        const int16_t *pcm_interleaved,
        size_t frame_count,
        uint32_t channel_count,
        int16_t *mono_out
);

#endif
