#include "dashcdg/soxr_resample.h"

#include <string.h>

#if defined(DASHCDG_HAVE_LIBSOXR)

#include <soxr.h>

int dashcdg_soxr_mono_i16_oneshot(
        const int16_t *in,
        size_t in_samples,
        uint32_t in_rate,
        int16_t *out,
        size_t out_samples,
        uint32_t out_rate
) {
    soxr_io_spec_t io;
    soxr_quality_spec_t qs;
    soxr_error_t err;
    size_t idone = 0U;
    size_t odone = 0U;

    if (in == NULL || out == NULL || in_samples == 0U || out_samples == 0U || in_rate == 0U || out_rate == 0U) {
        return -1;
    }
    if (in_rate == out_rate && in_samples == out_samples) {
        memcpy(out, in, in_samples * sizeof(int16_t));
        return 0;
    }

    io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    io.flags = SOXR_NO_DITHER;
    qs = soxr_quality_spec(SOXR_VHQ, SOXR_LINEAR_PHASE);

    err = soxr_oneshot(
            (double) in_rate,
            (double) out_rate,
            1U,
            in,
            in_samples,
            &idone,
            out,
            out_samples,
            &odone,
            &io,
            &qs,
            NULL
    );
    if (err != NULL || idone != in_samples || odone == 0U) {
        return -1;
    }

    if (odone < out_samples) {
        int16_t pad = out[odone - 1U];

        for (size_t i = odone; i < out_samples; ++i) {
            out[i] = pad;
        }
    }
    return 0;
}

#endif /* DASHCDG_HAVE_LIBSOXR */
