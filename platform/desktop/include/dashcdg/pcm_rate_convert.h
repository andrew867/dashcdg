#ifndef DASHCDG_PCM_RATE_CONVERT_H
#define DASHCDG_PCM_RATE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Band-limited-ish rate conversion for desktop codec adapters (float math).
 * Maps output sample j to input position j * in_rate / out_rate (seconds-aligned).
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
