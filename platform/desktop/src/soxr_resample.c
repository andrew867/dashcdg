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
    soxr_error_t cerr;
    soxr_t r;
    size_t id_used = 0U;
    size_t od_total = 0U;

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

    r = soxr_create((double) in_rate, (double) out_rate, 1U, &cerr, &io, &qs, NULL);
    if (r == NULL || cerr != NULL) {
        if (r != NULL) {
            soxr_delete(r);
        }
        return -1;
    }

    while (od_total < out_samples) {
        size_t id = 0U;
        size_t od = 0U;
        const int16_t *pin;
        size_t ilen;
        soxr_error_t pe;

        if (id_used < in_samples) {
            pin = in + id_used;
            ilen = in_samples - id_used;
        } else {
            pin = NULL;
            ilen = 0U;
        }

        pe = soxr_process(r, pin, ilen, &id, out + od_total, out_samples - od_total, &od);
        if (pe != NULL) {
            soxr_delete(r);
            return -1;
        }

        id_used += id;
        od_total += od;

        /* End-of-input flush: no more output available. */
        if (pin == NULL && od == 0U) {
            soxr_delete(r);
            return -1;
        }
    }

    soxr_delete(r);
    if (od_total != out_samples) {
        return -1;
    }
    return 0;
}

#endif /* DASHCDG_HAVE_LIBSOXR */
