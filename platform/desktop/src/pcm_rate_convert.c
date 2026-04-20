#include "dashcdg/pcm_rate_convert.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef DASHCDG_PCM_PI
#define DASHCDG_PCM_PI 3.14159265358979323846
#endif

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
        out[j] = dashcdg_f32_to_i16(acc);
    }
}

/*
 * Lanczos kernel (order a): band-limited reconstruction for arbitrary resample ratios.
 * Used for desktop TX capture and non–integer-rate paths where cubic interpolation aliases.
 */
static size_t dashcdg_pcm_output_frames_for_stream_position(uint64_t in_samples, uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == 0U || out_rate == 0U) {
        return 0U;
    }
    return (size_t)((in_samples * (uint64_t) out_rate + (uint64_t) in_rate - 1ULL) / (uint64_t) in_rate);
}

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
        out[j] = dashcdg_f32_to_i16((float) acc);
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

    /*
     * Fallback: Lanczos sinc (a=4) approximates SoX-quality band-limited resampling for
     * arbitrary ratios (e.g. 44.1 kHz microphone → 48 kHz session).
     */
    dashcdg_pcm_mono_resample_lanczos(in, in_len, in_rate, out, out_len, out_rate, 4);
}

void dashcdg_pcm_stereo_interleaved_to_mono48(
        const int16_t *pcm48_interleaved,
        size_t frame_count,
        int16_t *mono48_out
) {
    const size_t block_frames = 240U;
    size_t block_start;

    if (pcm48_interleaved == NULL || mono48_out == NULL) {
        return;
    }

    for (block_start = 0U; block_start < frame_count; block_start += block_frames) {
        size_t block_end = block_start + block_frames;
        double energy_l = 0.0;
        double energy_r = 0.0;
        double energy_mid = 0.0;
        double preferred_weight = 0.0;

        if (block_end > frame_count) {
            block_end = frame_count;
        }

        for (size_t i = block_start; i < block_end; ++i) {
            double l = (double) pcm48_interleaved[i * 2U];
            double r = (double) pcm48_interleaved[i * 2U + 1U];
            double mid = (l + r) * 0.5;

            energy_l += l * l;
            energy_r += r * r;
            energy_mid += mid * mid;
        }

        if (energy_l > 1.0 || energy_r > 1.0) {
            double peak_energy = energy_l > energy_r ? energy_l : energy_r;
            double mid_ratio_sq = energy_mid / peak_energy;

            /*
             * Keep normal mono sum for coherent stereo. When the mid channel loses
             * a lot of energy over a short window, blend toward the stronger channel
             * for the whole block instead of flipping source every sample.
             */
            if (mid_ratio_sq < (0.85 * 0.85)) {
                preferred_weight = ((0.85 * 0.85) - mid_ratio_sq) / ((0.85 * 0.85) - (0.25 * 0.25));
                if (preferred_weight < 0.0) {
                    preferred_weight = 0.0;
                }
                if (preferred_weight > 1.0) {
                    preferred_weight = 1.0;
                }
            }
        }

        for (size_t i = block_start; i < block_end; ++i) {
            double l = (double) pcm48_interleaved[i * 2U];
            double r = (double) pcm48_interleaved[i * 2U + 1U];
            double mid = (l + r) * 0.5;
            double preferred = energy_l >= energy_r ? l : r;
            double mixed = mid * (1.0 - preferred_weight) + preferred * preferred_weight;

            mono48_out[i] = dashcdg_f32_to_i16((float) mixed);
        }
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
}

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
) {
    size_t prepend;
    size_t ext_in;
    size_t ext_out;
    size_t skip_out;
    size_t outs_at_chunk_start;
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

    outs_at_chunk_start = dashcdg_pcm_output_frames_for_stream_position(stream_in_samples_before_chunk, in_rate, out_rate);
    ext_base_in = stream_in_samples_before_chunk > prepend ? stream_in_samples_before_chunk - prepend : 0ULL;
    outs_at_ext_base = dashcdg_pcm_output_frames_for_stream_position(ext_base_in, in_rate, out_rate);
    skip_out = outs_at_chunk_start > outs_at_ext_base ? outs_at_chunk_start - outs_at_ext_base : 0U;

    if (work_cap < ext_in || work_cap < ext_out || skip_out + out_frames > ext_out) {
        memset(out, 0, out_frames * 2U * sizeof(int16_t));
        goto update_tail;
    }

    memcpy(work_left, tail_l, prepend * sizeof(int16_t));
    for (i = 0U; i < in_frames; ++i) {
        work_left[prepend + i] = in[i * 2U];
    }
    dashcdg_pcm_mono_resample_cubic(work_left, ext_in, in_rate, work_right, ext_out, out_rate);
    for (k = 0U; k < out_frames; ++k) {
        out[k * 2U] = work_right[skip_out + k];
    }

    memcpy(work_left, tail_r, prepend * sizeof(int16_t));
    for (i = 0U; i < in_frames; ++i) {
        work_left[prepend + i] = in[i * 2U + 1U];
    }
    dashcdg_pcm_mono_resample_cubic(work_left, ext_in, in_rate, work_right, ext_out, out_rate);
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
            int32_t mid = (l + r) / 2;
            int32_t abs_l = l < 0 ? -l : l;
            int32_t abs_r = r < 0 ? -r : r;
            int32_t abs_mid = mid < 0 ? -mid : mid;
            int32_t peak = abs_l > abs_r ? abs_l : abs_r;

            if (abs_mid * 2 < peak) {
                mono_out[i] = (int16_t) (abs_l >= abs_r ? l : r);
            } else {
                mono_out[i] = (int16_t) mid;
            }
        }
    }
}
