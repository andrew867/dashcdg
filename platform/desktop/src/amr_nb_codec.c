#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "interf_enc.h"
#include "interf_dec.h"
#include "sp_enc.h"

#include "dashcdg/pcm_rate_convert.h"

#define DASHCDG_AMR_NB_PCM8K 160
#define DASHCDG_AMR_NB_PCM48K 960
#define DASHCDG_AMR_NB_RESAMPLE_WORK 1600

struct dashcdg_amr_nb_codec_ctx {
    void *codec;
    int16_t tail[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t tail_valid;
    uint64_t stream_samples;
    int16_t work_in[DASHCDG_AMR_NB_RESAMPLE_WORK];
    int16_t work_out[DASHCDG_AMR_NB_RESAMPLE_WORK];
};

void dashcdg_amr_nb_encoder_create(void **opaque) {
    struct dashcdg_amr_nb_codec_ctx *ctx;

    if (opaque == NULL) {
        return;
    }
    *opaque = NULL;
    ctx = (struct dashcdg_amr_nb_codec_ctx *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->codec = Encoder_Interface_init(0);
    if (ctx->codec == NULL) {
        free(ctx);
        return;
    }
    *opaque = ctx;
}

void dashcdg_amr_nb_encoder_destroy(void *opaque) {
    struct dashcdg_amr_nb_codec_ctx *ctx = (struct dashcdg_amr_nb_codec_ctx *) opaque;

    if (ctx == NULL) {
        return;
    }
    if (ctx->codec != NULL) {
        Encoder_Interface_exit(ctx->codec);
    }
    free(ctx);
}

int dashcdg_amr_nb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    struct dashcdg_amr_nb_codec_ctx *ctx = (struct dashcdg_amr_nb_codec_ctx *) opaque;
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];
    int n;

    if (ctx == NULL || ctx->codec == NULL || pcm48_960 == NULL || out == NULL || out_cap < 32U) {
        return 0;
    }
    dashcdg_pcm_mono_resample_overlap(
            ctx->tail,
            &ctx->tail_valid,
            ctx->stream_samples,
            pcm48_960,
            DASHCDG_AMR_NB_PCM48K,
            48000U,
            pcm8k,
            DASHCDG_AMR_NB_PCM8K,
            8000U,
            ctx->work_in,
            ctx->work_out,
            DASHCDG_AMR_NB_RESAMPLE_WORK
    );
    ctx->stream_samples += DASHCDG_AMR_NB_PCM48K;
    n = Encoder_Interface_Encode(ctx->codec, MR122, pcm8k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_nb_decoder_create(void **opaque) {
    struct dashcdg_amr_nb_codec_ctx *ctx;

    if (opaque == NULL) {
        return;
    }
    *opaque = NULL;
    ctx = (struct dashcdg_amr_nb_codec_ctx *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->codec = Decoder_Interface_init();
    if (ctx->codec == NULL) {
        free(ctx);
        return;
    }
    *opaque = ctx;
}

void dashcdg_amr_nb_decoder_destroy(void *opaque) {
    struct dashcdg_amr_nb_codec_ctx *ctx = (struct dashcdg_amr_nb_codec_ctx *) opaque;

    if (ctx == NULL) {
        return;
    }
    if (ctx->codec != NULL) {
        Decoder_Interface_exit(ctx->codec);
    }
    free(ctx);
}

int dashcdg_amr_nb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    struct dashcdg_amr_nb_codec_ctx *ctx = (struct dashcdg_amr_nb_codec_ctx *) opaque;
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];

    if (ctx == NULL || ctx->codec == NULL ||
            in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_NB_PCM48K) {
        return 0;
    }
    if (in_len > 64U) {
        return 0;
    }
    {
        unsigned char bits[64];
        memcpy(bits, in, in_len);
        Decoder_Interface_Decode(ctx->codec, bits, pcm8k, 0);
    }
    dashcdg_pcm_mono_resample_overlap(
            ctx->tail,
            &ctx->tail_valid,
            ctx->stream_samples,
            pcm8k,
            DASHCDG_AMR_NB_PCM8K,
            8000U,
            pcm48_960,
            DASHCDG_AMR_NB_PCM48K,
            48000U,
            ctx->work_in,
            ctx->work_out,
            DASHCDG_AMR_NB_RESAMPLE_WORK
    );
    ctx->stream_samples += DASHCDG_AMR_NB_PCM8K;
    return (int) DASHCDG_AMR_NB_PCM48K;
}
