#include "dashcdg/pcm_rate_convert.h"

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

    for (j = 0U; j < out_len; ++j) {
        float pos = ((float) j * (float) in_rate) / (float) out_rate;

        out[j] = dashcdg_mono_sample_cubic(in, in_len, pos);
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
        int32_t s = (int32_t) pcm48_interleaved[i * 2U] + (int32_t) pcm48_interleaved[i * 2U + 1U];

        mono48_out[i] = (int16_t) (s / 2);
    }
}
