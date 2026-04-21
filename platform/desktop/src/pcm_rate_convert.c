#include "dashcdg/pcm_rate_convert.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef DASHCDG_PCM_PI
#define DASHCDG_PCM_PI 3.14159265358979323846
#endif

#if defined(DASHCDG_HAVE_LIBSOXR)
#include "dashcdg/soxr_resample.h"
#endif

static size_t dashcdg_pcm_output_frames_for_stream_position(uint64_t in_samples, uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == 0U || out_rate == 0U) {
        return 0U;
    }
    return (size_t)((in_samples * (uint64_t) out_rate + (uint64_t) in_rate - 1ULL) / (uint64_t) in_rate);
}

/*
 * Exact-ratio low-pass FIR taps for the narrowband codec adapters.
 * These paths are the critical ones for v4 low-bitrate codecs:
 * 48 kHz -> 8 kHz (factor 6) and 48 kHz -> 16 kHz (factor 3).
 *
 * The previous cubic-only resampler performed interpolation without
 * anti-alias filtering, which lets high-frequency program material fold back
 * into the narrowband passband before AMR/EVRC/QCELP/SBC encode.
 */
static const float dashcdg_decimate_48k_to_8k_taps[] = {
        0.000000000000f, 0.000204078005f, 0.000750166130f, 0.001032662202f,
        -0.000000000000f, -0.003218354410f, -0.008517104454f, -0.014248135899f,
        -0.017246225235f, -0.013599598996f, 0.000000000000f, 0.024782381201f,
        0.058882296098f, 0.097259463930f, 0.132718638974f, 0.157783219474f,
        0.166833025959f, 0.157783219474f, 0.132718638974f, 0.097259463930f,
        0.058882296098f, 0.024782381201f, 0.000000000000f, -0.013599598996f,
        -0.017246225235f, -0.014248135899f, -0.008517104454f, -0.003218354410f,
        -0.000000000000f, 0.001032662202f, 0.000750166130f, 0.000204078005f,
        0.000000000000f
};

static const float dashcdg_decimate_48k_to_16k_taps[] = {
        -0.000000000000f, 0.000000000000f, 0.000749742778f, 0.001787614000f,
        -0.000000000000f, -0.005571207499f, -0.008512297873f, 0.000000000000f,
        0.017236492424f, 0.023541903179f, -0.000000000000f, -0.042900119258f,
        -0.058849066202f, 0.000000000000f, 0.132643739948f, 0.273134323835f,
        0.333477749338f, 0.273134323835f, 0.132643739948f, 0.000000000000f,
        -0.058849066202f, -0.042900119258f, -0.000000000000f, 0.023541903179f,
        0.017236492424f, 0.000000000000f, -0.008512297873f, -0.005571207499f,
        -0.000000000000f, 0.001787614000f, 0.000749742778f, 0.000000000000f,
        -0.000000000000f
};

static int16_t dashcdg_f32_to_i16(float x) {
    if (x > 32767.0f) {
        return 32767;
    }
    if (x < -32768.0f) {
        return -32768;
    }
    return (int16_t) (x < 0.0f ? x - 0.5f : x + 0.5f);
}

/*
 * Lanczos upsampling can exceed int16 peak on hot transients before quantization; a hard clip sounds
 * like a crude limiter. Above a knee, compress smoothly toward ±full scale without exceeding it.
 */
int16_t dashcdg_pcm_float_soft_limit_to_i16(float x) {
    const float knee = 28000.0f;
    const float lim = 32767.0f;
    float ax = fabsf(x);
    float sign = (x < 0.0f) ? -1.0f : 1.0f;

    if (ax <= knee) {
        return dashcdg_f32_to_i16(x);
    }
    {
        float over = ax - knee;
        float span = lim - knee;
        float target = knee + span * (1.0f - expf(-over / (span * 0.35f)));

        if (target > lim) {
            target = lim;
        }
        return dashcdg_f32_to_i16(sign * target);
    }
}

void dashcdg_pcm_interleaved_s16_soft_limit_inplace(int16_t *pcm, size_t frame_count, unsigned int channels) {
    size_t i;
    size_t n;

    if (pcm == NULL || frame_count == 0U || channels == 0U) {
        return;
    }
    n = frame_count * (size_t) channels;
    for (i = 0U; i < n; ++i) {
        pcm[i] = dashcdg_pcm_float_soft_limit_to_i16((float) pcm[i]);
    }
}

void dashcdg_pcm_interleaved_s16_gain_q15_inplace(
        int16_t *pcm,
        size_t frame_count,
        unsigned int channels,
        int32_t gain_q15
) {
    size_t i;
    size_t n;

    if (pcm == NULL || frame_count == 0U || channels == 0U || gain_q15 <= 0) {
        return;
    }
    if (gain_q15 > 32767) {
        gain_q15 = 32767;
    }
    n = frame_count * (size_t) channels;
    for (i = 0U; i < n; ++i) {
        int32_t y = ((int32_t) pcm[i] * gain_q15) >> 15;

        if (y > 32767) {
            y = 32767;
        } else if (y < -32768) {
            y = -32768;
        }
        pcm[i] = (int16_t) y;
    }
}

void dashcdg_pcm_hp80_biquad_reset(struct dashcdg_pcm_hp80_biquad_state *st) {
    if (st != NULL) {
        st->x1 = 0.0f;
        st->x2 = 0.0f;
        st->y1 = 0.0f;
        st->y2 = 0.0f;
    }
}

/*
 * Second-order Butterworth high-pass, fc=80 Hz, fs=48000 Hz, RBJ audio EQ cookbook, Q = 1/sqrt(2).
 */
static void dashcdg_pcm_hp80_biquad_process_float(
        struct dashcdg_pcm_hp80_biquad_state *st,
        float b0,
        float b1,
        float b2,
        float a1,
        float a2,
        float x,
        float *y_out
) {
    float y = b0 * x + b1 * st->x1 + b2 * st->x2 - a1 * st->y1 - a2 * st->y2;

    st->x2 = st->x1;
    st->x1 = x;
    st->y2 = st->y1;
    st->y1 = y;
    *y_out = y;
}

void dashcdg_pcm_hp80_process_mono(
        struct dashcdg_pcm_hp80_biquad_state *st,
        int16_t *samples,
        size_t frame_count
) {
    static const float b0 = 0.9926225427561189f;
    static const float b1 = -1.9852450855122379f;
    static const float b2 = 0.9926225427561189f;
    static const float a1 = -1.9851906578962613f;
    static const float a2 = 0.9852995131282146f;
    size_t i;

    if (st == NULL || samples == NULL || frame_count == 0U) {
        return;
    }

    for (i = 0U; i < frame_count; ++i) {
        float xf = (float) samples[i];
        float y;

        dashcdg_pcm_hp80_biquad_process_float(st, b0, b1, b2, a1, a2, xf, &y);
        samples[i] = dashcdg_pcm_float_soft_limit_to_i16(y);
    }
}

void dashcdg_pcm_hp80_process_stereo_interleaved(
        struct dashcdg_pcm_hp80_biquad_state *st_l,
        struct dashcdg_pcm_hp80_biquad_state *st_r,
        int16_t *interleaved,
        size_t frame_count
) {
    static const float b0 = 0.9926225427561189f;
    static const float b1 = -1.9852450855122379f;
    static const float b2 = 0.9926225427561189f;
    static const float a1 = -1.9851906578962613f;
    static const float a2 = 0.9852995131282146f;
    size_t i;

    if (st_l == NULL || st_r == NULL || interleaved == NULL || frame_count == 0U) {
        return;
    }

    for (i = 0U; i < frame_count; ++i) {
        float xl = (float) interleaved[i * 2U];
        float xr = (float) interleaved[i * 2U + 1U];
        float yl;
        float yr;

        dashcdg_pcm_hp80_biquad_process_float(st_l, b0, b1, b2, a1, a2, xl, &yl);
        dashcdg_pcm_hp80_biquad_process_float(st_r, b0, b1, b2, a1, a2, xr, &yr);
        interleaved[i * 2U] = dashcdg_pcm_float_soft_limit_to_i16(yl);
        interleaved[i * 2U + 1U] = dashcdg_pcm_float_soft_limit_to_i16(yr);
    }
}

static void dashcdg_pcm_mono_decimate_fir_exact_ratio(
        const int16_t *in,
        size_t in_len,
        int16_t *out,
        size_t out_len,
        uint32_t factor,
        const float *taps,
        size_t tap_count
) {
    size_t center = tap_count / 2U;
    size_t j;

    for (j = 0U; j < out_len; ++j) {
        size_t src = j * (size_t) factor;
        float acc = 0.0f;
        size_t k;

        for (k = 0U; k < tap_count; ++k) {
            ptrdiff_t idx = (ptrdiff_t) src + (ptrdiff_t) k - (ptrdiff_t) center;

            if (idx < 0) {
                idx = 0;
            } else if ((size_t) idx >= in_len) {
                idx = (ptrdiff_t) (in_len - 1U);
            }
            acc += (float) in[(size_t) idx] * taps[k];
        }
        out[j] = dashcdg_pcm_float_soft_limit_to_i16(acc);
    }
}

/*
 * Lanczos kernel (order a): band-limited reconstruction for arbitrary resample ratios.
 * Used for desktop TX capture and non–integer-rate paths where cubic interpolation aliases.
 */
static float dashcdg_lanczos_kernel(float x, int a) {
    float ax = fabsf(x);

    if (ax < 1e-7f) {
        return 1.0f;
    }
    if (ax >= (float) a) {
        return 0.0f;
    }
    {
        float p1 = (float) DASHCDG_PCM_PI * x;
        float p2 = p1 / (float) a;

        return (sinf(p1) / p1) * (sinf(p2) / p2);
    }
}

static void dashcdg_pcm_mono_resample_lanczos(
        const int16_t *in,
        size_t in_len,
        uint32_t in_rate,
        int16_t *out,
        size_t out_len,
        uint32_t out_rate,
        int a
) {
    size_t j;

    for (j = 0U; j < out_len; ++j) {
        double t = ((double) j * (double) in_rate) / (double) out_rate;
        double acc = 0.0;
        int ti = (int) floor(t);
        int i;
        int i0 = ti - a + 1;
        int i1 = ti + a;

        for (i = i0; i <= i1; ++i) {
            float w = dashcdg_lanczos_kernel((float) (t - (double) i), a);
            int ii = i;

            if (ii < 0) {
                ii = 0;
            } else if ((size_t) ii >= in_len) {
                ii = (int) (in_len - 1U);
            }
            acc += (double) in[(size_t) ii] * (double) w;
        }
        out[j] = dashcdg_pcm_float_soft_limit_to_i16((float) acc);
    }
}

void dashcdg_pcm_mono_resample_cubic(
        const int16_t *in,
        size_t in_len,
        uint32_t in_rate,
        int16_t *out,
        size_t out_len,
        uint32_t out_rate
) {

    if (in == NULL || out == NULL || in_len == 0U || out_len == 0U || in_rate == 0U || out_rate == 0U) {
        return;
    }

    if (in_rate == 48000U && out_rate == 8000U && out_len == in_len / 6U) {
        dashcdg_pcm_mono_decimate_fir_exact_ratio(
                in,
                in_len,
                out,
                out_len,
                6U,
                dashcdg_decimate_48k_to_8k_taps,
                sizeof(dashcdg_decimate_48k_to_8k_taps) / sizeof(dashcdg_decimate_48k_to_8k_taps[0])
        );
        return;
    }

    if (in_rate == 48000U && out_rate == 16000U && out_len == in_len / 3U) {
        dashcdg_pcm_mono_decimate_fir_exact_ratio(
                in,
                in_len,
                out,
                out_len,
                3U,
                dashcdg_decimate_48k_to_16k_taps,
                sizeof(dashcdg_decimate_48k_to_16k_taps) / sizeof(dashcdg_decimate_48k_to_16k_taps[0])
        );
        return;
    }

    if (in_rate == 8000U && out_rate == 48000U && out_len == in_len * 6U) {
        dashcdg_pcm_mono_resample_lanczos(in, in_len, in_rate, out, out_len, out_rate, 4);
        return;
    }

    if (in_rate == 16000U && out_rate == 48000U && out_len == in_len * 3U) {
        dashcdg_pcm_mono_resample_lanczos(in, in_len, in_rate, out, out_len, out_rate, 4);
        return;
    }

    if (in_rate == out_rate && in_len == out_len) {
        memcpy(out, in, out_len * sizeof(*out));
        return;
    }

#if defined(DASHCDG_HAVE_LIBSOXR)
    if (dashcdg_soxr_mono_i16_oneshot(in, in_len, in_rate, out, out_len, out_rate) != 0) {
        memset(out, 0, out_len * sizeof(*out));
    }
#else
    /*
     * Fallback: Lanczos sinc (a=4) approximates SoX-quality band-limited resampling for
     * arbitrary ratios (e.g. 44.1 kHz microphone → 48 kHz session).
     */
    dashcdg_pcm_mono_resample_lanczos(in, in_len, in_rate, out, out_len, out_rate, 4);
#endif
}

void dashcdg_pcm_stereo_interleaved_to_mono48(
        const int16_t *pcm48_interleaved,
        size_t frame_count,
        int16_t *mono48_out
) {
    size_t i;

    if (pcm48_interleaved == NULL || mono48_out == NULL) {
        return;
    }

    for (i = 0U; i < frame_count; ++i) {
        int32_t s = (int32_t) pcm48_interleaved[i * 2U] + (int32_t) pcm48_interleaved[i * 2U + 1U];

        mono48_out[i] = (int16_t) (s / 2);
    }
}

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
) {
    size_t i;

    if (in == NULL || out == NULL || work_left == NULL || work_right == NULL || in_frames == 0U || out_frames == 0U ||
        in_rate == 0U || out_rate == 0U) {
        return;
    }
    if (work_cap < in_frames || work_cap < out_frames) {
        /*
         * Caller bug or dimension slip; never leave out[] uninitialized (would be queued to speakers).
         */
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        return;
    }

    if (in_rate == out_rate && in_frames == out_frames) {
        memcpy(out, in, in_frames * 2U * sizeof(int16_t));
        return;
    }

#if defined(DASHCDG_HAVE_LIBSOXR)
    for (i = 0U; i < in_frames; ++i) {
        work_left[i] = in[i * 2U];
    }
    if (dashcdg_soxr_mono_i16_oneshot(work_left, in_frames, in_rate, work_right, out_frames, out_rate) != 0) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        return;
    }
    for (i = 0U; i < out_frames; ++i) {
        out[i * 2U] = work_right[i];
    }

    for (i = 0U; i < in_frames; ++i) {
        work_left[i] = in[i * 2U + 1U];
    }
    if (dashcdg_soxr_mono_i16_oneshot(work_left, in_frames, in_rate, work_right, out_frames, out_rate) != 0) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        return;
    }
    for (i = 0U; i < out_frames; ++i) {
        out[i * 2U + 1U] = work_right[i];
    }
#else
    for (i = 0U; i < in_frames; ++i) {
        work_left[i] = in[i * 2U];
    }
    dashcdg_pcm_mono_resample_cubic(work_left, in_frames, in_rate, work_right, out_frames, out_rate);
    for (i = 0U; i < out_frames; ++i) {
        out[i * 2U] = work_right[i];
    }

    for (i = 0U; i < in_frames; ++i) {
        work_left[i] = in[i * 2U + 1U];
    }
    dashcdg_pcm_mono_resample_cubic(work_left, in_frames, in_rate, work_right, out_frames, out_rate);
    for (i = 0U; i < out_frames; ++i) {
        out[i * 2U + 1U] = work_right[i];
    }
#endif
}

void dashcdg_pcm_stereo_interleaved_resample_overlap(
        int16_t *tail_l,
        int16_t *tail_r,
        size_t *tail_valid,
        uint64_t stream_in_samples_before_chunk,
        uint64_t stream_out_samples_before_chunk,
        const int16_t *in,
        size_t in_frames,
        uint32_t in_rate,
        int16_t *out,
        size_t out_frames,
        uint32_t out_rate,
        int16_t *work_left,
        int16_t *work_right,
        size_t work_cap
) {
    size_t prepend;
    size_t ext_in;
    size_t ext_out;
    size_t skip_out;
    size_t outs_at_ext_base;
    uint64_t ext_base_in;
    size_t i;
    size_t k;
    size_t overlap_max = (size_t) DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES;

    if (in == NULL || out == NULL || tail_l == NULL || tail_r == NULL || tail_valid == NULL ||
        work_left == NULL || work_right == NULL || in_frames == 0U || out_frames == 0U ||
        in_rate == 0U || out_rate == 0U) {
        return;
    }

    if (*tail_valid > overlap_max) {
        *tail_valid = 0U;
    }

    prepend = *tail_valid;

    if (in_rate == out_rate && in_frames == out_frames) {
        if (out != in) {
            memcpy(out, in, in_frames * 2U * sizeof(int16_t));
        }
        goto update_tail;
    }

    ext_in = prepend + in_frames;
    ext_out = (ext_in * (size_t) out_rate + (size_t) in_rate - 1U) / (size_t) in_rate;

    ext_base_in = stream_in_samples_before_chunk > prepend ? stream_in_samples_before_chunk - prepend : 0ULL;
    outs_at_ext_base = dashcdg_pcm_output_frames_for_stream_position(ext_base_in, in_rate, out_rate);
    skip_out = stream_out_samples_before_chunk > (uint64_t) outs_at_ext_base ?
            (size_t) (stream_out_samples_before_chunk - (uint64_t) outs_at_ext_base) : 0U;

    if (out_frames > ext_out) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        goto update_tail;
    }
    if (skip_out + out_frames > ext_out) {
        skip_out = ext_out - out_frames;
    }

    if (work_cap < ext_in || work_cap < ext_out || skip_out + out_frames > ext_out) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        goto update_tail;
    }

    memcpy(work_left, tail_l, prepend * sizeof(int16_t));
    for (i = 0U; i < in_frames; ++i) {
        work_left[prepend + i] = in[i * 2U];
    }
#if defined(DASHCDG_HAVE_LIBSOXR)
    if (dashcdg_soxr_mono_i16_oneshot(work_left, ext_in, in_rate, work_right, ext_out, out_rate) != 0) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        goto update_tail;
    }
#else
    dashcdg_pcm_mono_resample_cubic(work_left, ext_in, in_rate, work_right, ext_out, out_rate);
#endif
    for (k = 0U; k < out_frames; ++k) {
        out[k * 2U] = work_right[skip_out + k];
    }

    memcpy(work_left, tail_r, prepend * sizeof(int16_t));
    for (i = 0U; i < in_frames; ++i) {
        work_left[prepend + i] = in[i * 2U + 1U];
    }
#if defined(DASHCDG_HAVE_LIBSOXR)
    if (dashcdg_soxr_mono_i16_oneshot(work_left, ext_in, in_rate, work_right, ext_out, out_rate) != 0) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        goto update_tail;
    }
#else
    dashcdg_pcm_mono_resample_cubic(work_left, ext_in, in_rate, work_right, ext_out, out_rate);
#endif
    for (k = 0U; k < out_frames; ++k) {
        out[k * 2U + 1U] = work_right[skip_out + k];
    }

update_tail:
    {
        size_t start = in_frames >= overlap_max ? in_frames - overlap_max : 0U;
        size_t ncopy = in_frames >= overlap_max ? overlap_max : in_frames;

        for (i = 0U; i < ncopy; ++i) {
            tail_l[i] = in[(start + i) * 2U];
            tail_r[i] = in[(start + i) * 2U + 1U];
        }
        *tail_valid = ncopy;
    }
}

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
) {
    size_t prepend;
    size_t ext_in;
    size_t ext_out;
    size_t skip_out;
    size_t outs_at_chunk_start;
    size_t outs_at_ext_base;
    uint64_t ext_base_in;
    size_t i;
    size_t overlap_max = (size_t) DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES;

    if (tail == NULL || tail_valid == NULL || in == NULL || out == NULL || work_in == NULL || work_out == NULL ||
            in_frames == 0U || out_frames == 0U || in_rate == 0U || out_rate == 0U) {
        return;
    }

    if (*tail_valid > overlap_max) {
        *tail_valid = 0U;
    }

    prepend = *tail_valid;

    if (in_rate == out_rate && in_frames == out_frames) {
        if (out != in) {
            memcpy(out, in, in_frames * sizeof(int16_t));
        }
        goto update_tail;
    }

    ext_in = prepend + in_frames;
    ext_out = (ext_in * (size_t) out_rate + (size_t) in_rate - 1U) / (size_t) in_rate;

    outs_at_chunk_start = dashcdg_pcm_output_frames_for_stream_position(stream_in_samples_before_chunk, in_rate, out_rate);
    ext_base_in = stream_in_samples_before_chunk > prepend ? stream_in_samples_before_chunk - prepend : 0ULL;
    outs_at_ext_base = dashcdg_pcm_output_frames_for_stream_position(ext_base_in, in_rate, out_rate);
    skip_out = outs_at_chunk_start > outs_at_ext_base ? outs_at_chunk_start - outs_at_ext_base : 0U;

    if (out_frames > ext_out) {
        memset(out, 0, out_frames * sizeof(int16_t));
        goto update_tail;
    }
    if (skip_out + out_frames > ext_out) {
        skip_out = ext_out - out_frames;
    }

    if (work_cap < ext_in || work_cap < ext_out || skip_out + out_frames > ext_out) {
        memset(out, 0, out_frames * sizeof(int16_t));
        goto update_tail;
    }

    memcpy(work_in, tail, prepend * sizeof(int16_t));
    memcpy(work_in + prepend, in, in_frames * sizeof(int16_t));

    dashcdg_pcm_mono_resample_cubic(work_in, ext_in, in_rate, work_out, ext_out, out_rate);

    memcpy(out, work_out + skip_out, out_frames * sizeof(int16_t));

update_tail:
    {
        size_t start = in_frames >= overlap_max ? in_frames - overlap_max : 0U;
        size_t ncopy = in_frames >= overlap_max ? overlap_max : in_frames;

        for (i = 0U; i < ncopy; ++i) {
            tail[i] = in[start + i];
        }
        *tail_valid = ncopy;
    }
}

void dashcdg_pcm_interleaved_to_mono(
        const int16_t *pcm_interleaved,
        size_t frame_count,
        uint32_t channel_count,
        int16_t *mono_out
) {
    if (pcm_interleaved == NULL || mono_out == NULL || channel_count == 0U) {
        return;
    }

    if (channel_count == 1U) {
        memcpy(mono_out, pcm_interleaved, frame_count * sizeof(*mono_out));
        return;
    }

    if (channel_count >= 2U) {
        for (size_t i = 0U; i < frame_count; ++i) {
            int32_t l = (int32_t) pcm_interleaved[i * (size_t) channel_count];
            int32_t r = (int32_t) pcm_interleaved[i * (size_t) channel_count + 1U];
            mono_out[i] = (int16_t) ((l + r) / 2);
        }
    }
}
