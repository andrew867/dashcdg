#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "dashcdg/opus_codec.h"

/*
 * Regression: Opus encode/decode must preserve non-trivial programme energy at a
 * typical karaoke bitrate (64 kbit/s → VOICE policy in opus_codec.c).
 */
int main(void) {
    struct dashcdg_opus_encoder encoder;
    struct dashcdg_opus_decoder decoder;
    int16_t pcm[960];
    int16_t decoded[960];
    uint8_t packet[4000];
    int enc_bytes;
    int dec_samples;
    size_t i;
    double energy = 0.0;

    for (i = 0U; i < 960U; ++i) {
        double t = (double) i / 48000.0;

        pcm[i] = (int16_t) (2800.0 * sin(2.0 * 3.141592653589793 * 523.25 * t));
    }

    assert(dashcdg_opus_encoder_init(&encoder, 48000, 1, 20, 64000));
    assert(dashcdg_opus_decoder_init(&decoder, 48000, 1, 20));

    enc_bytes = dashcdg_opus_encode_frame(&encoder, pcm, packet, sizeof(packet));
    assert(enc_bytes > 0);

    memset(decoded, 0, sizeof(decoded));
    dec_samples = dashcdg_opus_decode_frame(&decoder, packet, (size_t) enc_bytes, decoded, 960U);
    assert(dec_samples > 0);

    for (i = 100U; i < 860U; ++i) {
        double s = (double) decoded[i];

        energy += s * s;
    }
    assert(energy > 5.0e8);

    dashcdg_opus_encoder_free(&encoder);
    dashcdg_opus_decoder_free(&decoder);
    return 0;
}
