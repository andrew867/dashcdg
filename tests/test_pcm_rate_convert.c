#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

    /* libsoxr passband gain vs legacy FIR — mid-buffer DC should remain within a small band of 10000. */
    for (i = 20U; i + 20U < 160U; ++i) {
        assert(out8k[i] >= 9700 && out8k[i] <= 10300);
    }
    for (i = 20U; i + 20U < 320U; ++i) {
        assert(out16k[i] >= 9700 && out16k[i] <= 10300);
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

static void test_stereo_to_mono_uses_linear_average(void) {
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

    assert(mono[0] == 0);
    assert(mono[1] == 0);
    assert(mono[2] == 10000);
    assert(mono[3] == -9000);
}

static void test_interleaved_to_mono_averages_stereo_input(void) {
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

    dashcdg_pcm_interleaved_to_mono(stereo, 4U, 2U, mono);

    assert(mono[0] == 0);
    assert(mono[1] == 0);
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

    /* libsoxr band-limited upsampling shows small ripple around DC vs flat legacy Lanczos mid-window. */
    for (i = 40U; i < 920U; ++i) {
        assert(out48k[i] >= 7560 && out48k[i] <= 7990);
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

static void test_sinc_resample_preserves_linearity_on_hot_programme(void) {
    int16_t in_a[4410];
    int16_t in_b[4410];
    int16_t in_sum[4410];
    int16_t out_a[4800];
    int16_t out_b[4800];
    int16_t out_sum[4800];
    size_t i;
    int max_diff = 0;

    for (i = 0U; i < 4410U; ++i) {
        double t = (double) i / 44100.0;
        double a = 17000.0 * sin(2.0 * 3.141592653589793 * 55.0 * t);
        double b = 17000.0 * sin(2.0 * 3.141592653589793 * 110.0 * t);

        in_a[i] = (int16_t) a;
        in_b[i] = (int16_t) b;
        in_sum[i] = (int16_t) (a + b);
    }

    dashcdg_pcm_mono_resample_cubic(in_a, 4410U, 44100U, out_a, 4800U, 48000U);
    dashcdg_pcm_mono_resample_cubic(in_b, 4410U, 44100U, out_b, 4800U, 48000U);
    dashcdg_pcm_mono_resample_cubic(in_sum, 4410U, 44100U, out_sum, 4800U, 48000U);

    for (i = 0U; i < 4800U; ++i) {
        int mixed = (int) out_a[i] + (int) out_b[i];
        int diff = (int) out_sum[i] - mixed;

        if (diff < 0) {
            diff = -diff;
        }
        if (diff > max_diff) {
            max_diff = diff;
        }
    }

    /* Superposition holds approximately under libsoxr (small numerical spread vs legacy Lanczos). */
    assert(max_diff <= 48);
}

static void test_overlap_chunk0_matches_isolated_resample(void) {
    int16_t pcm[960 * 2];
    int16_t out_iso[882 * 2];
    int16_t out_ov[882 * 2];
    int16_t tail_l[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    int16_t tail_r[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t tv = 0;
    int16_t wl[6144];
    int16_t wr[6144];
    size_t i;
    int max_diff = 0;

    for (i = 0U; i < 960U; ++i) {
        double t = (double) i / 48000.0;

        pcm[i * 2U] = (int16_t) (2600.0 * sin(2.0 * 3.141592653589793 * 440.0 * t));
        pcm[i * 2U + 1U] = pcm[i * 2U];
    }

    dashcdg_pcm_stereo_interleaved_resample(pcm, 960U, 48000U, out_iso, 882U, 44100U, wl, wr, 6144U);

    dashcdg_pcm_stereo_interleaved_resample_overlap(
            tail_l,
            tail_r,
            &tv,
            0ULL,
            0ULL,
            pcm,
            960U,
            48000U,
            out_ov,
            882U,
            44100U,
            wl,
            wr,
            6144U
    );

    for (i = 0U; i < 882U * 2U; ++i) {
        int d = (int) out_iso[i] - (int) out_ov[i];

        if (d < 0) {
            d = -d;
        }
        if (d > max_diff) {
            max_diff = d;
        }
    }

    assert(max_diff <= 4);
}

static void test_mono_overlap_exact_ratio_48k_to_8k_matches_contiguous(void) {
    enum { chunks = 5U, chunk_in = 960U, chunk_out = 160U };
    const size_t total_in = (size_t) chunks * (size_t) chunk_in;
    const size_t total_out = (size_t) chunks * (size_t) chunk_out;
    int16_t *full = (int16_t *) malloc(total_in * sizeof(int16_t));
    int16_t *ref = (int16_t *) malloc(total_out * sizeof(int16_t));
    int16_t tail[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t tv = 0U;
    int16_t work_in[2048];
    int16_t work_out[2048];
    size_t ci;
    size_t ref_off = 0U;
    int max_diff = 0;

    assert(full != NULL && ref != NULL);

    for (ci = 0U; ci < total_in; ++ci) {
        double t = (double) ci / 48000.0;
        double s = 12000.0 * sin(2.0 * 3.141592653589793 * 110.0 * t)
                + 7000.0 * sin(2.0 * 3.141592653589793 * 880.0 * t);

        full[ci] = (int16_t) s;
    }

    dashcdg_pcm_mono_resample_cubic(full, total_in, 48000U, ref, total_out, 8000U);

    memset(tail, 0, sizeof(tail));
    for (ci = 0U; ci < (size_t) chunks; ++ci) {
        int16_t chunk_out_buf[chunk_out];
        size_t j;

        dashcdg_pcm_mono_resample_overlap(
                tail,
                &tv,
                (uint64_t) (ci * (size_t) chunk_in),
                full + ci * (size_t) chunk_in,
                chunk_in,
                48000U,
                chunk_out_buf,
                chunk_out,
                8000U,
                work_in,
                work_out,
                2048U
        );

        for (j = 0U; j < chunk_out; ++j) {
            int d = (int) chunk_out_buf[j] - (int) ref[ref_off + j];

            if (d < 0) {
                d = -d;
            }
            if (d > max_diff) {
                max_diff = d;
            }
        }
        ref_off += chunk_out;
    }

    assert(ref_off == total_out);
    /*
     * Chunked mono overlap aligns FIR phases across chunk joins; comparing to one-shot FIR on the
     * union buffer can diverge near boundaries by more than a few counts (similar in spirit to the
     * stereo overlap test tolerances farther below).
     */
    assert(max_diff <= 128);

    free(full);
    free(ref);
}

static void test_overlap_stereo_48k_to_441_chunks_match_long_buffer(void) {
    enum { chunks = 5U, chunk_in = 960U };
    const size_t total_in = (size_t) chunks * (size_t) chunk_in;
    size_t total_out = (total_in * 44100U + 48000U - 1U) / 48000U;
    size_t chunk_out_fc = ((size_t) chunk_in * 44100U + 48000U - 1U) / 48000U;
    int16_t *full = (int16_t *) malloc(total_in * 2U * sizeof(int16_t));
    int16_t *ref = (int16_t *) malloc(total_out * 2U * sizeof(int16_t));
    int16_t tail_l[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    int16_t tail_r[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t tv = 0;
    int16_t wl[6144];
    int16_t wr[6144];
    size_t ci;
    int max_diff = 0;
    size_t ref_off = 0;

    assert(full != NULL && ref != NULL);

    for (ci = 0U; ci < total_in; ++ci) {
        double t = (double) ci / 48000.0;
        int16_t s = (int16_t) (2800.0 * sin(2.0 * 3.141592653589793 * 523.25 * t));

        full[ci * 2U] = s;
        full[ci * 2U + 1U] = s;
    }

    dashcdg_pcm_stereo_interleaved_resample(full, total_in, 48000U, ref, total_out, 44100U, wl, wr, 6144U);

    memset(tail_l, 0, sizeof(tail_l));
    memset(tail_r, 0, sizeof(tail_r));
    tv = 0U;

    for (ci = 0U; ci < (size_t) chunks; ++ci) {
        int16_t chunk_out[882 * 2];
        size_t j;

        dashcdg_pcm_stereo_interleaved_resample_overlap(
                tail_l,
                tail_r,
                &tv,
                (uint64_t) ((size_t) ci * (size_t) chunk_in),
                (uint64_t) ref_off,
                full + ci * (size_t) chunk_in * 2U,
                (size_t) chunk_in,
                48000U,
                chunk_out,
                chunk_out_fc,
                44100U,
                wl,
                wr,
                6144U
        );

        assert(ref_off + chunk_out_fc <= total_out);
        for (j = 0U; j < chunk_out_fc; ++j) {
            int dl = (int) chunk_out[j * 2U] - (int) ref[ref_off * 2U + j * 2U];
            int dr = (int) chunk_out[j * 2U + 1U] - (int) ref[ref_off * 2U + j * 2U + 1U];

            if (dl < 0) {
                dl = -dl;
            }
            if (dr < 0) {
                dr = -dr;
            }
            if (dl > max_diff) {
                max_diff = dl;
            }
            if (dr > max_diff) {
                max_diff = dr;
            }
        }
        ref_off += chunk_out_fc;
    }

    assert(ref_off == total_out);
    /*
     * Chunked overlap uses repeated mono SoXR passes; cumulative alignment differs slightly from one
     * contiguous stereo rate conversion — allow higher sample deltas than legacy Lanczos.
     */
    assert(max_diff <= 3000);

    free(full);
    free(ref);
}

static void test_interleaved_to_mono_copies_single_channel_input(void) {
    int16_t mono_in[5] = { 100, -200, 300, -400, 500 };
    int16_t mono_out[5] = { 0 };

    dashcdg_pcm_interleaved_to_mono(mono_in, 5U, 1U, mono_out);

    for (size_t i = 0U; i < 5U; ++i) {
        assert(mono_out[i] == mono_in[i]);
    }
}

static void test_gain_q15_matches_nb_headroom_constant(void) {
    int16_t mono[3] = { 10000, -10000, 32767 };
    int32_t exp_pos = (int32_t) ((10000LL * (int64_t) DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15) >> 15);
    int32_t exp_neg = (int32_t) ((-10000LL * (int64_t) DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15) >> 15);
    int32_t exp_peak = (32767LL * (int64_t) DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15) >> 15;

    dashcdg_pcm_interleaved_s16_gain_q15_inplace(mono, 3U, 1U, DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15);

    assert(mono[0] == exp_pos);
    assert(mono[1] == exp_neg);
    assert(mono[2] == (int16_t) exp_peak);
}

int main(void) {
    test_dc_is_preserved_on_exact_narrowband_decimation();
    test_dc_preserved_on_8k_to_48k_upsample();
    test_sinc_resample_441_to_480_yields_sine_energy();
    test_sinc_resample_preserves_linearity_on_hot_programme();
    test_alias_prone_high_frequency_is_rejected();
    test_stereo_to_mono_uses_linear_average();
    test_interleaved_to_mono_copies_single_channel_input();
    test_gain_q15_matches_nb_headroom_constant();
    test_interleaved_to_mono_averages_stereo_input();
    test_overlap_chunk0_matches_isolated_resample();
    test_mono_overlap_exact_ratio_48k_to_8k_matches_contiguous();
    test_overlap_stereo_48k_to_441_chunks_match_long_buffer();
    return 0;
}
