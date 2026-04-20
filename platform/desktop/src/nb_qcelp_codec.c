#include "dashcdg/nb_codec_adapters.h"

#include <stdlib.h>
#include <string.h>

#include "celp.h"
#include "coder.h"
#include "defines.h"
#include "basic_op.h"
#include "tty.h"

#include "dashcdg/pcm_rate_convert.h"

char *trans_fname = NULL;

struct dashcdg_qcelp13k_codec {
    struct ENCODER_MEM enc_mem;
    struct DECODER_MEM dec_mem;
    struct PACKET packet;
    struct CONTROL control;
    float in_workspace[LPCSIZE - FSIZE + LPCOFFSET + FSIZE];
    float out_speech[FSIZE];
    int encoder_ready;
    int decoder_ready;
    int16_t enc_tail48[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t enc_tail48_valid;
    uint64_t enc_stream48_samples;
    int16_t dec_tail8[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t dec_tail8_valid;
    uint64_t dec_stream8_samples;
    int16_t work_in[1600];
    int16_t work_out[1600];
};

static void dashcdg_float_mono_8k_to_pcm48_stereo(
        struct dashcdg_qcelp13k_codec *c,
        const float *mono_float,
        int16_t *pcm48_stereo,
        size_t stereo_samples
) {
    int16_t mono8k[FSIZE];
    int16_t mono48[960];
    size_t i;

    for (i = 0U; i < FSIZE; ++i) {
        float s = mono_float[i] * 4.0f;

        mono8k[i] = dashcdg_pcm_float_soft_limit_to_i16(s);
    }
    dashcdg_pcm_mono_resample_overlap(
            c->dec_tail8,
            &c->dec_tail8_valid,
            c->dec_stream8_samples,
            mono8k,
            FSIZE,
            8000U,
            mono48,
            960U,
            48000U,
            c->work_in,
            c->work_out,
            1600U
    );
    c->dec_stream8_samples += FSIZE;
    for (i = 0U; i < 960U; ++i) {
        pcm48_stereo[i * 2U] = mono48[i];
        pcm48_stereo[i * 2U + 1U] = mono48[i];
    }
    (void) stereo_samples;
}

int dashcdg_qcelp13k_encoder_create(void **out_ctx) {
    struct dashcdg_qcelp13k_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    c = (struct dashcdg_qcelp13k_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    memset(&c->enc_mem, 0, sizeof(c->enc_mem));
    memset(&c->dec_mem, 0, sizeof(c->dec_mem));
    memset(&c->packet, 0, sizeof(c->packet));
    memset(&c->control, 0, sizeof(c->control));
    c->control.min_rate = EIGHTH;
    c->control.max_rate = FULLRATE_VOICED;
    c->control.avg_rate = 14.4f;
    c->control.target_snr_thr = 10.0f;
    c->control.num_frames = UNLIMITED;
    c->control.pf_flag = YES;
    c->control.pitch_post = YES;
    c->control.celp_file_format = FORMAT_PACKET;
    trans_fname = NULL;
    tty_option = 0;
    memset(c->in_workspace, 0, sizeof(c->in_workspace));
    memset(c->out_speech, 0, sizeof(c->out_speech));
    initialize_encoder_and_decoder(&c->enc_mem, &c->dec_mem, &c->control);
    c->encoder_ready = 1;
    *out_ctx = c;
    return 1;
}

void dashcdg_qcelp13k_encoder_destroy(void *ctx) {
    struct dashcdg_qcelp13k_codec *c = (struct dashcdg_qcelp13k_codec *) ctx;

    if (c == NULL) {
        return;
    }
    if (c->encoder_ready || c->decoder_ready) {
        free_encoder_and_decoder(&c->enc_mem, &c->dec_mem);
        c->encoder_ready = 0;
        c->decoder_ready = 0;
    }
    free(c);
}

int dashcdg_qcelp13k_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
) {
    struct dashcdg_qcelp13k_codec *c = (struct dashcdg_qcelp13k_codec *) ctx;
    int16_t mono48[960];
    int16_t mono8k[FSIZE];
    size_t i;

    if (c == NULL || !c->encoder_ready || pcm48_interleaved == NULL || out == NULL) {
        return -1;
    }
    if (pcm_samples < 960U * 2U || out_max < DASHCDG_QCELP13K_FRAME_BYTES) {
        return -1;
    }
    dashcdg_pcm_stereo_interleaved_to_mono48(pcm48_interleaved, 960U, mono48);
    dashcdg_pcm_mono_resample_overlap(
            c->enc_tail48,
            &c->enc_tail48_valid,
            c->enc_stream48_samples,
            mono48,
            960U,
            48000U,
            mono8k,
            FSIZE,
            8000U,
            c->work_in,
            c->work_out,
            1600U
    );
    c->enc_stream48_samples += 960U;
    for (i = 0; i < FSIZE; ++i) {
        c->in_workspace[(LPCSIZE - FSIZE + LPCOFFSET) + i] = (float) mono8k[i] / 4.0f;
    }
    encoder(c->in_workspace, &c->packet, &c->control, &c->enc_mem, c->out_speech);
    {
        uint16_t *w = (uint16_t *) out;
        int j;

        for (j = 0; j < (int) WORDS_PER_PACKET; ++j) {
            w[j] = (uint16_t) ((unsigned) c->packet.data[j] & 0xFFFFU);
        }
    }
    for (i = 0; i < (size_t) (LPCSIZE - FSIZE + LPCOFFSET); ++i) {
        c->in_workspace[i] = c->in_workspace[i + FSIZE];
    }
    return (int) DASHCDG_QCELP13K_FRAME_BYTES;
}

int dashcdg_qcelp13k_decoder_create(void **out_ctx) {
    struct dashcdg_qcelp13k_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    c = (struct dashcdg_qcelp13k_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    memset(&c->enc_mem, 0, sizeof(c->enc_mem));
    memset(&c->dec_mem, 0, sizeof(c->dec_mem));
    memset(&c->packet, 0, sizeof(c->packet));
    memset(&c->control, 0, sizeof(c->control));
    c->control.min_rate = EIGHTH;
    c->control.max_rate = FULLRATE_VOICED;
    c->control.avg_rate = 14.4f;
    c->control.target_snr_thr = 10.0f;
    c->control.num_frames = UNLIMITED;
    c->control.pf_flag = YES;
    c->control.pitch_post = YES;
    trans_fname = NULL;
    tty_option = 0;
    initialize_encoder_and_decoder(&c->enc_mem, &c->dec_mem, &c->control);
    c->decoder_ready = 1;
    *out_ctx = c;
    return 1;
}

void dashcdg_qcelp13k_decoder_destroy(void *ctx) {
    dashcdg_qcelp13k_encoder_destroy(ctx);
}

int dashcdg_qcelp13k_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
) {
    struct dashcdg_qcelp13k_codec *c = (struct dashcdg_qcelp13k_codec *) ctx;
    const uint16_t *w;
    int j;

    if (c == NULL || !c->decoder_ready || in == NULL || pcm48_interleaved == NULL) {
        return -1;
    }
    if (in_len < DASHCDG_QCELP13K_FRAME_BYTES || pcm_samples_max < 960U * 2U) {
        return -1;
    }
    w = (const uint16_t *) in;
    for (j = 0; j < (int) WORDS_PER_PACKET; ++j) {
        c->packet.data[j] = (int) w[j];
    }
    decoder(c->out_speech, &c->packet, &c->control, &c->dec_mem);
    dashcdg_float_mono_8k_to_pcm48_stereo(c, c->out_speech, pcm48_interleaved, 960U * 2U);
    return 960;
}

/*
 * decode.c references fer_sim when trans_fname is set; lpc.c calls usage on
 * invalid window types. The CLI simulator (fer_sim.c / celp13k.c) is not
 * linked into the desktop library, so provide minimal stubs.
 */
void fer_sim(int *rate) {
    (void) rate;
}

void usage(struct CONTROL *control) {
    (void) control;
}

Word16 round_l(Word32 L_var1) {
    Word32 L_Prod = L_add(L_var1, (Word32) 0x00008000L);

    return extract_h(L_Prod);
}
