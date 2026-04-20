#include "dashcdg/pcm_soxr_stream.h"

#include <string.h>

#if defined(DASHCDG_HAVE_LIBSOXR)

#include <soxr.h>

static soxr_t g_soxr;
static uint32_t g_in_sr;
static uint32_t g_out_sr;

void dashcdg_pcm_soxr_stream_reset(void) {
    if (g_soxr != NULL) {
        soxr_delete(g_soxr);
        g_soxr = NULL;
    }
    g_in_sr = 0U;
    g_out_sr = 0U;
}

int dashcdg_pcm_soxr_stream_available(void) {
    return 1;
}

int dashcdg_pcm_soxr_stream_ensure(uint32_t in_sr, uint32_t out_sr) {
    soxr_io_spec_t io;
    soxr_quality_spec_t qs;
    soxr_error_t err;
    soxr_t r;

    if (in_sr == 0U || out_sr == 0U || in_sr == out_sr) {
        dashcdg_pcm_soxr_stream_reset();
        return 0;
    }

    if (g_soxr != NULL && g_in_sr == in_sr && g_out_sr == out_sr) {
        return 1;
    }

    dashcdg_pcm_soxr_stream_reset();

    io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    io.flags = SOXR_NO_DITHER;
    qs = soxr_quality_spec(SOXR_VHQ, SOXR_LINEAR_PHASE);

    r = soxr_create((double) in_sr, (double) out_sr, 2U, &err, &io, &qs, NULL);
    if (r == NULL || err != NULL) {
        if (r != NULL) {
            soxr_delete(r);
        }
        return 0;
    }

    g_soxr = r;
    g_in_sr = in_sr;
    g_out_sr = out_sr;
    return 1;
}

int dashcdg_pcm_soxr_stream_process_interleaved(
        const int16_t *in,
        size_t in_frames,
        int16_t *out,
        size_t out_frames
) {
    size_t idone = 0U;
    size_t odone = 0U;
    soxr_error_t e;

    if (g_soxr == NULL || in == NULL || out == NULL || in_frames == 0U || out_frames == 0U) {
        return 0;
    }

    e = soxr_process(g_soxr, in, in_frames, &idone, out, out_frames, &odone);
    if (e != NULL || idone != in_frames || odone != out_frames) {
        return 0;
    }
    return 1;
}

#else /* !DASHCDG_HAVE_LIBSOXR */

void dashcdg_pcm_soxr_stream_reset(void) {
}

int dashcdg_pcm_soxr_stream_available(void) {
    return 0;
}

int dashcdg_pcm_soxr_stream_ensure(uint32_t in_sr, uint32_t out_sr) {
    (void) in_sr;
    (void) out_sr;
    return 0;
}

int dashcdg_pcm_soxr_stream_process_interleaved(
        const int16_t *in,
        size_t in_frames,
        int16_t *out,
        size_t out_frames
) {
    (void) in;
    (void) in_frames;
    (void) out;
    (void) out_frames;
    return 0;
}

#endif
