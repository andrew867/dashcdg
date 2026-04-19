#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "dashcdg/pcm_rate_convert.h"

static void test_dc_is_preserved_on_exact_narrowband_decimation(void) {
    int16_t in48k[960];
    int16_t out8k[160];
    int16_t out16k[320];
    size_t i;

    for (i = 0U; i < 960U; ++i) {
        in48k[i] = 10000;
    }

    dashcdg_pcm_mono_resample_cubic(in48k, 960U, 48000U, out8k, 160U, 8000U);
    dashcdg_pcm_mono_resample_cubic(in48k, 960U, 48000U, out16k, 320U, 16000U);

    for (i = 0U; i < 160U; ++i) {
        assert(out8k[i] >= 9999 && out8k[i] <= 10001);
    }
    for (i = 0U; i < 320U; ++i) {
        assert(out16k[i] >= 9999 && out16k[i] <= 10001);
    }
}

static void test_alias_prone_high_frequency_is_rejected(void) {
    int16_t in48k[960U * 4U];
    int16_t out8k[(960U * 4U) / 6U];
    int16_t out16k[(960U * 4U) / 3U];
    int64_t sum_abs_8k = 0;
    int64_t sum_abs_16k = 0;
    size_t i;

    for (i = 0U; i < sizeof(in48k) / sizeof(in48k[0]); ++i) {
        in48k[i] = (i & 1U) == 0U ? 32767 : -32768;
    }

    dashcdg_pcm_mono_resample_cubic(
            in48k,
            sizeof(in48k) / sizeof(in48k[0]),
            48000U,
            out8k,
            sizeof(out8k) / sizeof(out8k[0]),
            8000U
    );
    dashcdg_pcm_mono_resample_cubic(
            in48k,
            sizeof(in48k) / sizeof(in48k[0]),
            48000U,
            out16k,
            sizeof(out16k) / sizeof(out16k[0]),
            16000U
    );

    for (i = 20U; i + 20U < sizeof(out8k) / sizeof(out8k[0]); ++i) {
        int32_t sample = out8k[i];
        sum_abs_8k += sample < 0 ? -(int64_t) sample : (int64_t) sample;
    }
    for (i = 20U; i + 20U < sizeof(out16k) / sizeof(out16k[0]); ++i) {
        int32_t sample = out16k[i];
        sum_abs_16k += sample < 0 ? -(int64_t) sample : (int64_t) sample;
    }

    assert(sum_abs_8k <= (int64_t) ((sizeof(out8k) / sizeof(out8k[0])) - 40U) * 4);
    assert(sum_abs_16k <= (int64_t) ((sizeof(out16k) / sizeof(out16k[0])) - 40U) * 8);
}

static void test_stereo_to_mono_avoids_phase_cancellation_collapse(void) {
    int16_t stereo[8];
    int16_t mono[4];

    stereo[0] = 12000;
    stereo[1] = -12000;
    stereo[2] = -14000;
    stereo[3] = 14000;
    stereo[4] = 10000;
    stereo[5] = 10000;
    stereo[6] = -9000;
    stereo[7] = -9000;

    dashcdg_pcm_stereo_interleaved_to_mono48(stereo, 4U, mono);

    assert(abs(mono[0]) >= 6000);
    assert(abs(mono[1]) >= 7000);
    assert(mono[2] == 10000);
    assert(mono[3] == -9000);
}

static void test_dc_preserved_on_8k_to_48k_upsample(void) {
    int16_t in8k[160];
    int16_t out48k[960];
    size_t i;

    for (i = 0U; i < 160U; ++i) {
        in8k[i] = 7777;
    }

    dashcdg_pcm_mono_resample_cubic(in8k, 160U, 8000U, out48k, 960U, 48000U);

    for (i = 40U; i < 920U; ++i) {
        assert(out48k[i] >= 7600 && out48k[i] <= 7950);
    }
}

static void test_sinc_resample_441_to_480_yields_sine_energy(void) {
    int16_t in441[441];
    int16_t out480[480];
    size_t i;
    double e = 0.0;

    for (i = 0U; i < 441U; ++i) {
        double t = (double) i / 44100.0;
        in441[i] = (int16_t) (2500.0 * sin(2.0 * 3.141592653589793 * 440.0 * t));
    }

    dashcdg_pcm_mono_resample_cubic(in441, 441U, 44100U, out480, 480U, 48000U);

    for (i = 40U; i < 440U; ++i) {
        double s = (double) out480[i];

        e += s * s;
    }
    assert(e > 1.0e8);
}

static void test_interleaved_to_mono_copies_single_channel_input(void) {
    int16_t mono_in[5] = { 100, -200, 300, -400, 500 };
    int16_t mono_out[5] = { 0 };

    dashcdg_pcm_interleaved_to_mono(mono_in, 5U, 1U, mono_out);

    for (size_t i = 0U; i < 5U; ++i) {
        assert(mono_out[i] == mono_in[i]);
    }
}

int main(void) {
    test_dc_is_preserved_on_exact_narrowband_decimation();
    test_dc_preserved_on_8k_to_48k_upsample();
    test_sinc_resample_441_to_480_yields_sine_energy();
    test_alias_prone_high_frequency_is_rejected();
    test_stereo_to_mono_avoids_phase_cancellation_collapse();
    test_interleaved_to_mono_copies_single_channel_input();
    return 0;
}
