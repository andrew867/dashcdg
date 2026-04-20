#ifndef DASHCDG_PCM_RATE_CONVERT_H
#define DASHCDG_PCM_RATE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/* RX streaming SRC: prepend this many past input samples so Lanczos sees continuity across frames. */
#define DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES 96

/*
 * High-quality rate conversion for desktop.
 * With DASHCDG_HAVE_LIBSOXR (MinGW + vendored libsoxr): libsoxr one-shot SRC (VHQ / linear phase).
 * Otherwise: legacy polyphase FIR + Lanczos paths (same public API).
 * Public name is historical (`_cubic`); cubics are not used.
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

/*
 * Stereo interleaved int16; work_left/work_right must each hold max(in_frames, out_frames) samples.
 */
void dashcdg_pcm_stereo_interleaved_resample(
        const int16_t *in,
        size_t in_frames,
        uint32_t in_rate,
        int16_t *out,
        size_t out_frames,
        uint32_t out_rate,
        int16_t *work_left,
        int16_t *work_right,
        size_t work_cap
);

/*
 * Same as dashcdg_pcm_stereo_interleaved_resample but keeps tail_l/tail_r (length *tail_valid)
 * from the previous chunk so band-limited resampling does not reset phase at every packet.
 * tail_* must hold at most DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES samples per channel; reset
 * *tail_valid to 0 after reconfigure or stream discontinuity.
 */
void dashcdg_pcm_stereo_interleaved_resample_overlap(
        int16_t *tail_l,
        int16_t *tail_r,
        size_t *tail_valid,
        uint64_t stream_in_samples_before_chunk,
        const int16_t *in,
        size_t in_frames,
        uint32_t in_rate,
        int16_t *out,
        size_t out_frames,
        uint32_t out_rate,
        int16_t *work_left,
        int16_t *work_right,
        size_t work_cap
);

void dashcdg_pcm_mono_resample_overlap(
        int16_t *tail,
        size_t *tail_valid,
        uint64_t stream_in_samples_before_chunk,
        const int16_t *in,
        size_t in_frames,
        uint32_t in_rate,
        int16_t *out,
        size_t out_frames,
        uint32_t out_rate,
        int16_t *work_in,
        int16_t *work_out,
        size_t work_cap
);

#endif
