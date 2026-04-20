#ifndef DASHCDG_SOXR_RESAMPLE_H
#define DASHCDG_SOXR_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#if defined(DASHCDG_HAVE_LIBSOXR)

/*
 * One-shot PCM rate conversion using libsoxr (VHQ / linear phase, int16).
 * in_samples / out_samples are mono sample counts (one channel).
 */
int dashcdg_soxr_mono_i16_oneshot(
        const int16_t *in,
        size_t in_samples,
        uint32_t in_rate,
        int16_t *out,
        size_t out_samples,
        uint32_t out_rate
);

#endif /* DASHCDG_HAVE_LIBSOXR */

#endif /* DASHCDG_SOXR_RESAMPLE_H */
