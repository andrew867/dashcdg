#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "typedef.h"
#include "enc_if.h"
#include "dec_if.h" /* NB_SERIAL_MAX, _lost_frame */

#include "dashcdg/pcm_rate_convert.h"

#define DASHCDG_AMR_WB_PCM16K 320
#define DASHCDG_AMR_WB_PCM48K 960
#define DASHCDG_AMR_WB_MODE 8
#define DASHCDG_AMR_WB_RESAMPLE_WORK 1600

struct dashcdg_amr_wb_codec_ctx {
    void *codec;
    int16_t tail[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t tail_valid;
    uint64_t stream_samples;
    int16_t work_in[DASHCDG_AMR_WB_RESAMPLE_WORK];
    int16_t work_out[DASHCDG_AMR_WB_RESAMPLE_WORK];
};

void dashcdg_amr_wb_encoder_create(void **opaque) {
    struct dashcdg_amr_wb_codec_ctx *ctx;

    if (opaque == NULL) {
        return;
    }
    *opaque = NULL;
    ctx = (struct dashcdg_amr_wb_codec_ctx *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->codec = E_IF_init();
    if (ctx->codec == NULL) {
        free(ctx);
        return;
    }
    *opaque = ctx;
}

void dashcdg_amr_wb_encoder_destroy(void *opaque) {
    struct dashcdg_amr_wb_codec_ctx *ctx = (struct dashcdg_amr_wb_codec_ctx *) opaque;

    if (ctx == NULL) {
        return;
    }
    if (ctx->codec != NULL) {
        E_IF_exit(ctx->codec);
    }
    free(ctx);
}

int dashcdg_amr_wb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    struct dashcdg_amr_wb_codec_ctx *ctx = (struct dashcdg_amr_wb_codec_ctx *) opaque;
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];
    int n;

    if (ctx == NULL || ctx->codec == NULL || pcm48_960 == NULL || out == NULL || out_cap < (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    dashcdg_pcm_mono_resample_overlap(
            ctx->tail,
            &ctx->tail_valid,
            ctx->stream_samples,
            pcm48_960,
            DASHCDG_AMR_WB_PCM48K,
            48000U,
            pcm16k,
            DASHCDG_AMR_WB_PCM16K,
            16000U,
            ctx->work_in,
            ctx->work_out,
            DASHCDG_AMR_WB_RESAMPLE_WORK
    );
    ctx->stream_samples += DASHCDG_AMR_WB_PCM48K;
    n = E_IF_encode(ctx->codec, (Word16) DASHCDG_AMR_WB_MODE, pcm16k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_wb_decoder_create(void **opaque) {
    struct dashcdg_amr_wb_codec_ctx *ctx;

    if (opaque == NULL) {
        return;
    }
    *opaque = NULL;
    ctx = (struct dashcdg_amr_wb_codec_ctx *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->codec = D_IF_init();
    if (ctx->codec == NULL) {
        free(ctx);
        return;
    }
    *opaque = ctx;
}

void dashcdg_amr_wb_decoder_destroy(void *opaque) {
    struct dashcdg_amr_wb_codec_ctx *ctx = (struct dashcdg_amr_wb_codec_ctx *) opaque;

    if (ctx == NULL) {
        return;
    }
    if (ctx->codec != NULL) {
        D_IF_exit(ctx->codec);
    }
    free(ctx);
}

int dashcdg_amr_wb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    struct dashcdg_amr_wb_codec_ctx *ctx = (struct dashcdg_amr_wb_codec_ctx *) opaque;
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];

    if (ctx == NULL || ctx->codec == NULL ||
            in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_WB_PCM48K) {
        return 0;
    }
    if (in_len > (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    {
        UWord8 bits[NB_SERIAL_MAX];
        memcpy(bits, in, in_len);
        D_IF_decode(ctx->codec, bits, pcm16k, _good_frame);
    }
    dashcdg_pcm_mono_resample_overlap(
            ctx->tail,
            &ctx->tail_valid,
            ctx->stream_samples,
            pcm16k,
            DASHCDG_AMR_WB_PCM16K,
            16000U,
            pcm48_960,
            DASHCDG_AMR_WB_PCM48K,
            48000U,
            ctx->work_in,
            ctx->work_out,
            DASHCDG_AMR_WB_RESAMPLE_WORK
    );
    ctx->stream_samples += DASHCDG_AMR_WB_PCM16K;
    return (int) DASHCDG_AMR_WB_PCM48K;
}

int dashcdg_amr_wb_decoder_run_lost(void *opaque, int16_t *pcm48_960, size_t pcm_cap_samples)
{
    struct dashcdg_amr_wb_codec_ctx *ctx = (struct dashcdg_amr_wb_codec_ctx *) opaque;
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];
    UWord8 bits[NB_SERIAL_MAX];

    if (ctx == NULL || ctx->codec == NULL || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_WB_PCM48K) {
        return 0;
    }
    memset(bits, 0, sizeof(bits));
    D_IF_decode(ctx->codec, bits, pcm16k, (Word32) _lost_frame);
    dashcdg_pcm_mono_resample_overlap(
            ctx->tail,
            &ctx->tail_valid,
            ctx->stream_samples,
            pcm16k,
            DASHCDG_AMR_WB_PCM16K,
            16000U,
            pcm48_960,
            DASHCDG_AMR_WB_PCM48K,
            48000U,
            ctx->work_in,
            ctx->work_out,
            DASHCDG_AMR_WB_RESAMPLE_WORK
    );
    ctx->stream_samples += DASHCDG_AMR_WB_PCM16K;
    return (int) DASHCDG_AMR_WB_PCM48K;
}
