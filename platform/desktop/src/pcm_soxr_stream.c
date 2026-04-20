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
    size_t id_used = 0U;
    size_t od_total = 0U;
    unsigned int guard = 0U;

    if (g_soxr == NULL || in == NULL || out == NULL || in_frames == 0U || out_frames == 0U) {
        return 0;
    }

    /*
     * libsoxr streaming API often requires multiple soxr_process calls per chunk (partial input
     * consumption / partial output); the previous single-call + equality check caused systematic
     * RX decode failures whenever session rate != device rate — no PCM queued, claim_audio_start
     * wedged on timeline vs ring, CDG drained behind fake audio back-pressure.
     */
    while (id_used < in_frames || od_total < out_frames) {
        size_t id = 0U;
        size_t od = 0U;
        const int16_t *pin;
        size_t ilen;
        soxr_error_t e;

        if (id_used < in_frames) {
            pin = in + id_used * 2U;
            ilen = in_frames - id_used;
        } else {
            pin = NULL;
            ilen = 0U;
        }

        e = soxr_process(g_soxr, pin, ilen, &id, out + od_total * 2U, out_frames - od_total, &od);
        if (e != NULL) {
            return 0;
        }

        id_used += id;
        od_total += od;

        if (pin == NULL && od == 0U) {
            return 0;
        }
        if (od_total > out_frames || id_used > in_frames) {
            return 0;
        }
        if (++guard > 100000U) {
            return 0;
        }
    }

    return (id_used == in_frames && od_total == out_frames) ? 1 : 0;
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
