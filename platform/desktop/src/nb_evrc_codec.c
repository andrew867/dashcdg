#include "dashcdg/nb_codec_adapters.h"

#include <stdlib.h>
#include <string.h>

#include "evrcc.h"

#include "dashcdg/pcm_rate_convert.h"

#define EVRC_FRAME8K 160
#define PCM48_FRAME 960
#define EVRC_RESAMPLE_WORK 1600

struct dashcdg_evrc_codec {
    void *enc;
    void *dec;
    int16_t enc_tail48[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t enc_tail48_valid;
    uint64_t enc_stream48_samples;
    int16_t dec_tail8[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t dec_tail8_valid;
    uint64_t dec_stream8_samples;
    int16_t work_in[EVRC_RESAMPLE_WORK];
    int16_t work_out[EVRC_RESAMPLE_WORK];
};

static int g_dashcdg_evrc_decoder_sentinel;

static struct dashcdg_evrc_codec *g_dashcdg_evrc_shared_encoder;
static unsigned int g_dashcdg_evrc_shared_encoder_refs;
static struct dashcdg_evrc_codec *g_dashcdg_evrc_shared_decoder;
static unsigned int g_dashcdg_evrc_shared_decoder_refs;

static void dashcdg_mono8k_to_pcm48_stereo(
        struct dashcdg_evrc_codec *c,
        const int16_t *mono8k,
        int16_t *pcm48,
        size_t stereo_samples
) {
    int16_t mono48[PCM48_FRAME];
    size_t i;

    dashcdg_pcm_mono_resample_overlap(
            c->dec_tail8,
            &c->dec_tail8_valid,
            c->dec_stream8_samples,
            mono8k,
            (size_t) EVRC_FRAME8K,
            8000U,
            mono48,
            PCM48_FRAME,
            48000U,
            c->work_in,
            c->work_out,
            EVRC_RESAMPLE_WORK
    );
    c->dec_stream8_samples += EVRC_FRAME8K;
    for (i = 0U; i < PCM48_FRAME; ++i) {
        size_t ix = i * 2U;

        pcm48[ix] = mono48[i];
        pcm48[ix + 1U] = mono48[i];
    }
    (void) stereo_samples;
}

int dashcdg_evrc_encoder_create(void **out_ctx) {
    struct dashcdg_evrc_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    if (g_dashcdg_evrc_shared_encoder != NULL) {
        g_dashcdg_evrc_shared_encoder_refs++;
        g_dashcdg_evrc_shared_encoder->enc_tail48_valid = 0U;
        g_dashcdg_evrc_shared_encoder->enc_stream48_samples = 0U;
        *out_ctx = g_dashcdg_evrc_shared_encoder;
        return 1;
    }
    c = (struct dashcdg_evrc_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    c->enc = evrc_encoder_init(4, 4, 1);
    if (c->enc == NULL) {
        free(c);
        return 0;
    }
    g_dashcdg_evrc_shared_encoder = c;
    g_dashcdg_evrc_shared_encoder_refs = 1U;
    *out_ctx = c;
    return 1;
}

void dashcdg_evrc_encoder_destroy(void *ctx) {
    struct dashcdg_evrc_codec *c = (struct dashcdg_evrc_codec *) ctx;

    if (c == NULL) {
        return;
    }
    if (c == g_dashcdg_evrc_shared_encoder) {
        if (g_dashcdg_evrc_shared_encoder_refs > 0U) {
            g_dashcdg_evrc_shared_encoder_refs--;
        }
        /*
         * The vendored EVRC codec is built around global static state. Keep the singleton alive for
         * the lifetime of the process instead of tearing it down on every hot-switch.
         */
        return;
    }
    if (c->enc != NULL) {
        evrc_encoder_uninit(c->enc);
    }
    if (c->dec != NULL) {
        evrc_decoder_uninit(c->dec);
    }
    free(c);
}

int dashcdg_evrc_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
) {
    struct dashcdg_evrc_codec *c = (struct dashcdg_evrc_codec *) ctx;
    int16_t mono48[PCM48_FRAME];
    int16_t mono8k[EVRC_FRAME8K];
    int n;

    if (c == NULL || c->enc == NULL || pcm48_interleaved == NULL || out == NULL || out_max == 0U) {
        return -1;
    }
    if (pcm_samples < PCM48_FRAME * 2U) {
        return -1;
    }
    dashcdg_pcm_stereo_interleaved_to_mono48(pcm48_interleaved, PCM48_FRAME, mono48);
    dashcdg_pcm_mono_resample_overlap(
            c->enc_tail48,
            &c->enc_tail48_valid,
            c->enc_stream48_samples,
            mono48,
            PCM48_FRAME,
            48000U,
            mono8k,
            (size_t) EVRC_FRAME8K,
            8000U,
            c->work_in,
            c->work_out,
            EVRC_RESAMPLE_WORK
    );
    c->enc_stream48_samples += PCM48_FRAME;
    n = evrc_encoder_encode_to_packet(c->enc, mono8k, EVRC_FRAME8K, out, out_max);
    if (n <= 0) {
        return -1;
    }
    return n;
}

int dashcdg_evrc_decoder_create(void **out_ctx) {
    struct dashcdg_evrc_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    if (g_dashcdg_evrc_shared_decoder != NULL) {
        g_dashcdg_evrc_shared_decoder_refs++;
        g_dashcdg_evrc_shared_decoder->dec_tail8_valid = 0U;
        g_dashcdg_evrc_shared_decoder->dec_stream8_samples = 0U;
        *out_ctx = g_dashcdg_evrc_shared_decoder;
        return 1;
    }
    c = (struct dashcdg_evrc_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    /*
     * The vendored EVRC decoder initializes global/static decode state but returns NULL.
     * Its decode entrypoints still reject a NULL context pointer, even though they do not
     * dereference it. Keep a stable non-NULL sentinel as the wrapper-visible decoder handle.
     */
    (void) evrc_decoder_init();
    c->dec = &g_dashcdg_evrc_decoder_sentinel;
    g_dashcdg_evrc_shared_decoder = c;
    g_dashcdg_evrc_shared_decoder_refs = 1U;
    *out_ctx = c;
    return 1;
}

void dashcdg_evrc_decoder_destroy(void *ctx) {
    struct dashcdg_evrc_codec *c = (struct dashcdg_evrc_codec *) ctx;

    if (c == NULL) {
        return;
    }
    if (c == g_dashcdg_evrc_shared_decoder) {
        if (g_dashcdg_evrc_shared_decoder_refs > 0U) {
            g_dashcdg_evrc_shared_decoder_refs--;
        }
        return;
    }
    dashcdg_evrc_encoder_destroy(ctx);
}

int dashcdg_evrc_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
) {
    struct dashcdg_evrc_codec *c = (struct dashcdg_evrc_codec *) ctx;
    int16_t mono8k[EVRC_FRAME8K];
    int got;

    if (c == NULL || c->dec == NULL || in == NULL || in_len == 0U || pcm48_interleaved == NULL) {
        return -1;
    }
    if (pcm_samples_max < PCM48_FRAME * 2U) {
        return -1;
    }
    got = evrc_decoder_decode_from_packet(c->dec, in, in_len, mono8k, EVRC_FRAME8K);
    if (got < (int) (EVRC_FRAME8K * (int) sizeof(int16_t))) {
        return -1;
    }
    dashcdg_mono8k_to_pcm48_stereo(c, mono8k, pcm48_interleaved, PCM48_FRAME * 2U);
    return (int) PCM48_FRAME;
}
