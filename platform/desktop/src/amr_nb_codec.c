#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "interf_enc.h"
#include "interf_dec.h"
#include "sp_enc.h"

#include "dashcdg/pcm_rate_convert.h"

#define DASHCDG_AMR_NB_PCM8K 160
#define DASHCDG_AMR_NB_PCM48K 960

void dashcdg_amr_nb_encoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = Encoder_Interface_init(0);
}

void dashcdg_amr_nb_encoder_destroy(void *opaque) {
    if (opaque != NULL) {
        Encoder_Interface_exit(opaque);
    }
}

int dashcdg_amr_nb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];
    int n;

    if (opaque == NULL || pcm48_960 == NULL || out == NULL || out_cap < 32U) {
        return 0;
    }
    dashcdg_pcm_mono_resample_cubic(
            pcm48_960,
            DASHCDG_AMR_NB_PCM48K,
            48000U,
            pcm8k,
            DASHCDG_AMR_NB_PCM8K,
            8000U
    );
    n = Encoder_Interface_Encode(opaque, MR122, pcm8k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_nb_decoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = Decoder_Interface_init();
}

void dashcdg_amr_nb_decoder_destroy(void *opaque) {
    if (opaque != NULL) {
        Decoder_Interface_exit(opaque);
    }
}

int dashcdg_amr_nb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];

    if (opaque == NULL || in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_NB_PCM48K) {
        return 0;
    }
    if (in_len > 64U) {
        return 0;
    }
    {
        unsigned char bits[64];
        memcpy(bits, in, in_len);
        Decoder_Interface_Decode(opaque, bits, pcm8k, 0);
    }
    dashcdg_pcm_mono_resample_cubic(
            pcm8k,
            DASHCDG_AMR_NB_PCM8K,
            8000U,
            pcm48_960,
            DASHCDG_AMR_NB_PCM48K,
            48000U
    );
    return (int) DASHCDG_AMR_NB_PCM48K;
}
