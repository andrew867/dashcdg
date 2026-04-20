#ifndef DASHCDG_PCM_RATE_CONVERT_H
#define DASHCDG_PCM_RATE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Butterworth biquad high-pass @ 80 Hz (session 48 kHz). State is per channel; reset on codec
 * / stream discontinuity so the IIR does not carry unrelated history across sessions.
 */
struct dashcdg_pcm_hp80_biquad_state {
    float x1, x2, y1, y2;
};

void dashcdg_pcm_hp80_biquad_reset(struct dashcdg_pcm_hp80_biquad_state *st);

/*
 * Apply 80 Hz HP to mono or stereo-interleaved PCM in place (for narrowband codec paths).
 */
void dashcdg_pcm_hp80_process_mono(
        struct dashcdg_pcm_hp80_biquad_state *st,
        int16_t *samples,
        size_t frame_count
);

void dashcdg_pcm_hp80_process_stereo_interleaved(
        struct dashcdg_pcm_hp80_biquad_state *st_l,
        struct dashcdg_pcm_hp80_biquad_state *st_r,
        int16_t *interleaved,
        size_t frame_count
);

/*
 * Soft saturation toward ±full scale (same knee as Lanczos peak tamer). Use on int16 programme
 * blocks to reduce hard-clip crackle on hot transients (narrowband paths, post-SRC, etc.).
 */
int16_t dashcdg_pcm_float_soft_limit_to_i16(float x);

void dashcdg_pcm_interleaved_s16_soft_limit_inplace(int16_t *pcm, size_t frame_count, unsigned int channels);

/*
 * Fixed Q15 gain in [1, 32767] applied to interleaved samples: out = clamp(s * gain_q15 / 32768).
 * DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15 is ~-3 dB (0.707) for speech-codec headroom on hot music.
 */
#define DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15 23170

void dashcdg_pcm_interleaved_s16_gain_q15_inplace(
        int16_t *pcm,
        size_t frame_count,
        unsigned int channels,
        int32_t gain_q15
);

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
