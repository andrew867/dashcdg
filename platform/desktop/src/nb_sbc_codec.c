#include "dashcdg/nb_codec_adapters.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "sbc/sbc.h"

#define SBC_PCM_FRAME48 960U
#define SBC_PCM_MONO16 320U

struct dashcdg_bt_sbc_codec {
    sbc_t enc;
    sbc_t dec;
    int enc_ok;
    int dec_ok;
};

static void dashcdg_pcm48_stereo_to_mono16_avg3(
        const int16_t *pcm48,
        size_t stereo_samples,
        int16_t *mono16,
        size_t mono_count
) {
    size_t i;

    for (i = 0; i < mono_count; ++i) {
        size_t base = i * 3U;
        int32_t acc = 0;
        size_t k;

        if (base + 2U >= stereo_samples / 2U) {
            break;
        }
        for (k = 0; k < 3U; ++k) {
            size_t ix = (base + k) * 2U;

            acc += (int32_t) pcm48[ix] + (int32_t) pcm48[ix + 1U];
        }
        mono16[i] = (int16_t) (acc / 6);
    }
    for (; i < mono_count; ++i) {
        mono16[i] = 0;
    }
}

static void dashcdg_mono16_hold3_to_pcm48_stereo(const int16_t *mono16, int16_t *pcm48, size_t stereo_samples) {
    size_t i;
    size_t pairs = stereo_samples / 2U;

    for (i = 0; i < pairs; ++i) {
        int16_t s = mono16[i / 3U];
        size_t ix = i * 2U;

        pcm48[ix] = s;
        pcm48[ix + 1U] = s;
    }
}

static int dashcdg_sbc_open_common(sbc_t *sbc, int encoder) {
    int err;

    memset(sbc, 0, sizeof(*sbc));
    err = sbc_init(sbc, 0L);
    if (err != 0) {
        return 0;
    }
    sbc->frequency = SBC_FREQ_16000;
    sbc->mode = SBC_MODE_MONO;
    sbc->subbands = SBC_SB_8;
    sbc->blocks = SBC_BLK_4;
    sbc->allocation = SBC_AM_LOUDNESS;
    sbc->bitpool = 24;
    sbc->endian = SBC_LE;
    (void) encoder;
    err = sbc_reinit(sbc, 0L);
    return err == 0 ? 1 : 0;
}

int dashcdg_bt_sbc_encoder_create(void **out_ctx) {
    struct dashcdg_bt_sbc_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    c = (struct dashcdg_bt_sbc_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    if (!dashcdg_sbc_open_common(&c->enc, 1)) {
        free(c);
        return 0;
    }
    c->enc_ok = 1;
    *out_ctx = c;
    return 1;
}

void dashcdg_bt_sbc_encoder_destroy(void *ctx) {
    struct dashcdg_bt_sbc_codec *c = (struct dashcdg_bt_sbc_codec *) ctx;

    if (c == NULL) {
        return;
    }
    if (c->enc_ok) {
        sbc_finish(&c->enc);
        c->enc_ok = 0;
    }
    if (c->dec_ok) {
        sbc_finish(&c->dec);
        c->dec_ok = 0;
    }
    free(c);
}

int dashcdg_bt_sbc_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
) {
    struct dashcdg_bt_sbc_codec *c = (struct dashcdg_bt_sbc_codec *) ctx;
    int16_t mono16[SBC_PCM_MONO16];
    size_t codesize;
    size_t pos = 1U;
    uint8_t nframes = 0U;
    const uint8_t *pcm_bytes;
    size_t pcm_off = 0U;

    if (c == NULL || !c->enc_ok || pcm48_interleaved == NULL || out == NULL || out_max < 8U) {
        return -1;
    }
    if (pcm_samples < SBC_PCM_FRAME48 * 2U) {
        return -1;
    }
    dashcdg_pcm48_stereo_to_mono16_avg3(pcm48_interleaved, pcm_samples, mono16, SBC_PCM_MONO16);
    codesize = sbc_get_codesize(&c->enc);
    if (codesize == 0U || codesize > sizeof(mono16)) {
        return -1;
    }
    pcm_bytes = (const uint8_t *) mono16;
    while (pcm_off + codesize <= sizeof(mono16) && pos + 2U + sbc_get_frame_length(&c->enc) <= out_max && nframes < 8U) {
        ssize_t w = 0;
        ssize_t encsz;

        encsz = sbc_encode(&c->enc, pcm_bytes + pcm_off, codesize, out + pos + 1U, out_max - pos - 1U, &w);
        if (encsz <= 0 || w <= 0) {
            return -1;
        }
        if ((size_t) w > 255U) {
            return -1;
        }
        out[pos] = (uint8_t) w;
        pos += 1U + (size_t) w;
        pcm_off += codesize;
        nframes++;
    }
    if (nframes == 0U) {
        return -1;
    }
    out[0] = nframes;
    return (int) pos;
}

int dashcdg_bt_sbc_decoder_create(void **out_ctx) {
    struct dashcdg_bt_sbc_codec *c;

    if (out_ctx == NULL) {
        return 0;
    }
    c = (struct dashcdg_bt_sbc_codec *) calloc(1, sizeof(*c));
    if (c == NULL) {
        return 0;
    }
    if (!dashcdg_sbc_open_common(&c->dec, 0)) {
        free(c);
        return 0;
    }
    c->dec_ok = 1;
    *out_ctx = c;
    return 1;
}

void dashcdg_bt_sbc_decoder_destroy(void *ctx) {
    dashcdg_bt_sbc_encoder_destroy(ctx);
}

int dashcdg_bt_sbc_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
) {
    struct dashcdg_bt_sbc_codec *c = (struct dashcdg_bt_sbc_codec *) ctx;
    int16_t mono16[SBC_PCM_MONO16];
    size_t pcmo = 0U;
    size_t p = 0U;
    uint8_t n;
    size_t codesize;

    if (c == NULL || !c->dec_ok || in == NULL || pcm48_interleaved == NULL) {
        return -1;
    }
    if (in_len < 1U || pcm_samples_max < SBC_PCM_FRAME48 * 2U) {
        return -1;
    }
    n = in[0];
    p = 1U;
    codesize = sbc_get_codesize(&c->dec);
    if (codesize == 0U || n == 0U || n > 8U) {
        return -1;
    }
    memset(mono16, 0, sizeof(mono16));
    while (n > 0U && p < in_len) {
        size_t flen = in[p];
        size_t written = 0U;
        ssize_t r;

        if (flen == 0U || p + 1U + flen > in_len) {
            return -1;
        }
        if (pcmo + codesize > sizeof(mono16)) {
            return -1;
        }
        r = sbc_decode(&c->dec, in + p + 1U, flen, (uint8_t *) mono16 + pcmo, sizeof(mono16) - pcmo, &written);
        if (r < 0 || written != codesize) {
            return -1;
        }
        pcmo += written / sizeof(int16_t);
        p += 1U + flen;
        n--;
    }
    if (pcmo < SBC_PCM_MONO16) {
        return -1;
    }
    dashcdg_mono16_hold3_to_pcm48_stereo(mono16, pcm48_interleaved, SBC_PCM_FRAME48 * 2U);
    return (int) SBC_PCM_FRAME48;
}
