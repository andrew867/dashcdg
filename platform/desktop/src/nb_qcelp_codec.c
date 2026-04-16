#include "dashcdg/nb_codec_adapters.h"

#include <stdlib.h>
#include <string.h>

#include "celp.h"
#include "coder.h"
#include "defines.h"
#include "basic_op.h"
#include "tty.h"

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
};

static void dashcdg_pcm48_stereo_to_mono8k_avg6(
        const int16_t *pcm48,
        size_t stereo_samples,
        int16_t *mono8k,
        size_t mono_count
) {
    size_t i;

    for (i = 0; i < mono_count; ++i) {
        size_t base = i * 6U;
        int32_t acc = 0;
        size_t k;

        if (base + 5U >= stereo_samples / 2U) {
            break;
        }
        for (k = 0; k < 6U; ++k) {
            size_t ix = (base + k) * 2U;

            acc += (int32_t) pcm48[ix] + (int32_t) pcm48[ix + 1U];
        }
        mono8k[i] = (int16_t) (acc / 12);
    }
    for (; i < mono_count; ++i) {
        mono8k[i] = 0;
    }
}

static void dashcdg_float_mono_to_pcm48_stereo(const float *mono, int16_t *pcm48, size_t stereo_samples) {
    size_t i;
    size_t pairs = stereo_samples / 2U;

    for (i = 0; i < pairs; ++i) {
        float s = mono[i / 6U] * 4.0f;
        int32_t v;
        size_t ix = i * 2U;

        if (s > 32767.0f) {
            s = 32767.0f;
        } else if (s < -32768.0f) {
            s = -32768.0f;
        }
        v = (int32_t) (s < 0 ? s - 0.5f : s + 0.5f);
        pcm48[ix] = (int16_t) v;
        pcm48[ix + 1U] = (int16_t) v;
    }
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
    int16_t mono8k[FSIZE];
    size_t i;

    if (c == NULL || !c->encoder_ready || pcm48_interleaved == NULL || out == NULL) {
        return -1;
    }
    if (pcm_samples < 960U * 2U || out_max < DASHCDG_QCELP13K_FRAME_BYTES) {
        return -1;
    }
    dashcdg_pcm48_stereo_to_mono8k_avg6(pcm48_interleaved, pcm_samples, mono8k, FSIZE);
    for (i = 0; i < (size_t) (LPCSIZE - FSIZE + LPCOFFSET); ++i) {
        c->in_workspace[i] = 0.0f;
    }
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
    dashcdg_float_mono_to_pcm48_stereo(c->out_speech, pcm48_interleaved, 960U * 2U);
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
