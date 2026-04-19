#include "dashcdg/pcm_rate_convert.h"

#include <stdlib.h>
#include <string.h>

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

static float dashcdg_catmull_rom4(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;

    return 0.5f *
            ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                    (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static int16_t dashcdg_f32_to_i16(float x) {
    if (x > 32767.0f) {
        return 32767;
    }
    if (x < -32768.0f) {
        return -32768;
    }
    return (int16_t) (x < 0.0f ? x - 0.5f : x + 0.5f);
}

static int16_t dashcdg_mono_sample_cubic(const int16_t *in, size_t in_len, float pos) {
    float idx = pos;

    if (in_len == 0U) {
        return 0;
    }
    if (in_len == 1U) {
        return in[0];
    }
    if (idx < 0.0f) {
        idx = 0.0f;
    }
    if (idx > (float) (in_len - 1U)) {
        idx = (float) (in_len - 1U);
    }
    {
        size_t i = (size_t) idx;
        float t = idx - (float) i;
        size_t i0 = i > 0U ? i - 1U : 0U;
        size_t i1 = i;
        size_t i2 = i + 1U < in_len ? i + 1U : in_len - 1U;
        size_t i3 = i + 2U < in_len ? i + 2U : in_len - 1U;
        float p0 = (float) in[i0];
        float p1 = (float) in[i1];
        float p2 = (float) in[i2];
        float p3 = (float) in[i3];

        return dashcdg_f32_to_i16(dashcdg_catmull_rom4(p0, p1, p2, p3, t));
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
        out[j] = dashcdg_f32_to_i16(acc);
    }
}

static void dashcdg_pcm_mono_lowpass_filter(
        const int16_t *in,
        size_t in_len,
        int16_t *out,
        const float *taps,
        size_t tap_count
) {
    size_t center = tap_count / 2U;
    size_t j;

    for (j = 0U; j < in_len; ++j) {
        float acc = 0.0f;
        size_t k;

        for (k = 0U; k < tap_count; ++k) {
            ptrdiff_t idx = (ptrdiff_t) j + (ptrdiff_t) k - (ptrdiff_t) center;

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

void dashcdg_pcm_mono_resample_cubic(
        const int16_t *in,
        size_t in_len,
        uint32_t in_rate,
        int16_t *out,
        size_t out_len,
        uint32_t out_rate
) {
    size_t j;

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

    for (j = 0U; j < out_len; ++j) {
        float pos = ((float) j * (float) in_rate) / (float) out_rate;

        out[j] = dashcdg_mono_sample_cubic(in, in_len, pos);
    }

    if (in_rate == 8000U && out_rate == 48000U) {
        int16_t *tmp = (int16_t *) malloc(out_len * sizeof(*tmp));

        if (tmp != NULL) {
            memcpy(tmp, out, out_len * sizeof(*tmp));
            dashcdg_pcm_mono_lowpass_filter(
                    tmp,
                    out_len,
                    out,
                    dashcdg_decimate_48k_to_8k_taps,
                    sizeof(dashcdg_decimate_48k_to_8k_taps) / sizeof(dashcdg_decimate_48k_to_8k_taps[0])
            );
            free(tmp);
        }
    } else if (in_rate == 16000U && out_rate == 48000U) {
        int16_t *tmp = (int16_t *) malloc(out_len * sizeof(*tmp));

        if (tmp != NULL) {
            memcpy(tmp, out, out_len * sizeof(*tmp));
            dashcdg_pcm_mono_lowpass_filter(
                    tmp,
                    out_len,
                    out,
                    dashcdg_decimate_48k_to_16k_taps,
                    sizeof(dashcdg_decimate_48k_to_16k_taps) / sizeof(dashcdg_decimate_48k_to_16k_taps[0])
            );
            free(tmp);
        }
    }
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
        int32_t l = (int32_t) pcm48_interleaved[i * 2U];
        int32_t r = (int32_t) pcm48_interleaved[i * 2U + 1U];
        int32_t mid = (l + r) / 2;
        int32_t abs_l = l < 0 ? -l : l;
        int32_t abs_r = r < 0 ? -r : r;
        int32_t abs_mid = mid < 0 ? -mid : mid;
        int32_t peak = abs_l > abs_r ? abs_l : abs_r;

        /*
         * Straight stereo averaging is mathematically correct for coherent mono content,
         * but wide karaoke/music masters can carry enough L/R phase difference to cancel
         * badly when folded to mono. That makes every narrowband mono codec sound broken
         * even before encode. Keep the normal mid mix when it retains most of the channel
         * energy; otherwise fall back to the stronger channel for this sample.
         */
        if (abs_mid * 2 < peak) {
            mono48_out[i] = (int16_t) (abs_l >= abs_r ? l : r);
        } else {
            mono48_out[i] = (int16_t) mid;
        }
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
