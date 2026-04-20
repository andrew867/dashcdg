#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "typedef.h"
#include "enc_if.h"
#include "dec_if.h"

#include "dashcdg/pcm_rate_convert.h"

#define DASHCDG_AMR_WB_PCM16K 320
#define DASHCDG_AMR_WB_PCM48K 960
#define DASHCDG_AMR_WB_MODE 8
#define DASHCDG_AMR_WB_RESAMPLE_WORK 1600

static int16_t g_amr_wb_enc_tail48[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
static size_t g_amr_wb_enc_tail48_valid;
static uint64_t g_amr_wb_enc_stream48_samples;
static int16_t g_amr_wb_dec_tail16[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
static size_t g_amr_wb_dec_tail16_valid;
static uint64_t g_amr_wb_dec_stream16_samples;
static int16_t g_amr_wb_work_in[DASHCDG_AMR_WB_RESAMPLE_WORK];
static int16_t g_amr_wb_work_out[DASHCDG_AMR_WB_RESAMPLE_WORK];

void dashcdg_amr_wb_encoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    g_amr_wb_enc_tail48_valid = 0U;
    g_amr_wb_enc_stream48_samples = 0U;
    *opaque = E_IF_init();
}

void dashcdg_amr_wb_encoder_destroy(void *opaque) {
    if (opaque != NULL) {
        E_IF_exit(opaque);
    }
}

int dashcdg_amr_wb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];
    int n;

    if (opaque == NULL || pcm48_960 == NULL || out == NULL || out_cap < (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    dashcdg_pcm_mono_resample_overlap(
            g_amr_wb_enc_tail48,
            &g_amr_wb_enc_tail48_valid,
            g_amr_wb_enc_stream48_samples,
            pcm48_960,
            DASHCDG_AMR_WB_PCM48K,
            48000U,
            pcm16k,
            DASHCDG_AMR_WB_PCM16K,
            16000U,
            g_amr_wb_work_in,
            g_amr_wb_work_out,
            DASHCDG_AMR_WB_RESAMPLE_WORK
    );
    g_amr_wb_enc_stream48_samples += DASHCDG_AMR_WB_PCM48K;
    n = E_IF_encode(opaque, (Word16) DASHCDG_AMR_WB_MODE, pcm16k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_wb_decoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    g_amr_wb_dec_tail16_valid = 0U;
    g_amr_wb_dec_stream16_samples = 0U;
    *opaque = D_IF_init();
}

void dashcdg_amr_wb_decoder_destroy(void *opaque) {
    if (opaque != NULL) {
        D_IF_exit(opaque);
    }
}

int dashcdg_amr_wb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];

    if (opaque == NULL || in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_WB_PCM48K) {
        return 0;
    }
    if (in_len > (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    {
        UWord8 bits[NB_SERIAL_MAX];
        memcpy(bits, in, in_len);
        D_IF_decode(opaque, bits, pcm16k, _good_frame);
    }
    dashcdg_pcm_mono_resample_overlap(
            g_amr_wb_dec_tail16,
            &g_amr_wb_dec_tail16_valid,
            g_amr_wb_dec_stream16_samples,
            pcm16k,
            DASHCDG_AMR_WB_PCM16K,
            16000U,
            pcm48_960,
            DASHCDG_AMR_WB_PCM48K,
            48000U,
            g_amr_wb_work_in,
            g_amr_wb_work_out,
            DASHCDG_AMR_WB_RESAMPLE_WORK
    );
    g_amr_wb_dec_stream16_samples += DASHCDG_AMR_WB_PCM16K;
    return (int) DASHCDG_AMR_WB_PCM48K;
}
