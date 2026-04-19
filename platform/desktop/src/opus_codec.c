#include "dashcdg/opus_codec.h"

#include <string.h>

#if defined(DASHCDG_DESKTOP_NO_OPUS)

int dashcdg_opus_encoder_init(
        struct dashcdg_opus_encoder *encoder,
        int sample_rate,
        int channels,
        int frame_ms,
        int bitrate_bps
) {
    (void) bitrate_bps;
    if (encoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }
    memset(encoder, 0, sizeof(*encoder));
    encoder->frame_size = (sample_rate * frame_ms) / 1000;
    encoder->sample_rate = sample_rate;
    encoder->channels = channels;
    encoder->bitrate_bps = 0;
    encoder->encoder = NULL;
    return 0;
}

void dashcdg_opus_encoder_free(struct dashcdg_opus_encoder *encoder) {
    if (encoder == NULL) {
        return;
    }
    memset(encoder, 0, sizeof(*encoder));
}

int dashcdg_opus_encode_frame(
        struct dashcdg_opus_encoder *encoder,
        const int16_t *pcm,
        uint8_t *output,
        size_t output_size
) {
    (void) pcm;
    (void) output;
    (void) output_size;
    if (encoder == NULL) {
        return -1;
    }
    return -1;
}

int dashcdg_opus_decoder_init(
        struct dashcdg_opus_decoder *decoder,
        int sample_rate,
        int channels,
        int frame_ms
) {
    if (decoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_size = (sample_rate * frame_ms) / 1000;
    decoder->sample_rate = sample_rate;
    decoder->channels = channels;
    decoder->decoder = NULL;
    return 0;
}

void dashcdg_opus_decoder_free(struct dashcdg_opus_decoder *decoder) {
    if (decoder == NULL) {
        return;
    }
    memset(decoder, 0, sizeof(*decoder));
}

int dashcdg_opus_decode_frame(
        struct dashcdg_opus_decoder *decoder,
        const uint8_t *input,
        size_t input_size,
        int16_t *pcm_output,
        size_t pcm_output_samples
) {
    (void) input;
    (void) input_size;
    (void) pcm_output;
    (void) pcm_output_samples;
    if (decoder == NULL) {
        return -1;
    }
    return -1;
}

#else /* !DASHCDG_DESKTOP_NO_OPUS */

int dashcdg_opus_encoder_init(
        struct dashcdg_opus_encoder *encoder,
        int sample_rate,
        int channels,
        int frame_ms,
        int bitrate_bps
) {
    int err;

    if (encoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }

    memset(encoder, 0, sizeof(*encoder));
    encoder->frame_size = (sample_rate * frame_ms) / 1000;
    encoder->bitrate_bps = bitrate_bps;
    encoder->sample_rate = sample_rate;
    encoder->channels = channels;
    encoder->encoder = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
    if (encoder->encoder == NULL || err != OPUS_OK) {
        encoder->encoder = NULL;
        return 0;
    }

    opus_encoder_ctl(encoder->encoder, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(encoder->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder->encoder, OPUS_SET_VBR_CONSTRAINT(1));
    /*
     * High complexity + in-band FEC can exceed small UDP payloads and peg CPU under load;
     * keep frames compact and encoding cheap for real-time desktop streaming.
     */
    opus_encoder_ctl(encoder->encoder, OPUS_SET_COMPLEXITY(5));
    /*
     * Karaoke default: moderate bitrates favour speech (intelligibility); higher bitrates
     * favour music programme. Policy: docs/specs/opus-desktop-encoding-policy.md
     */
    /*
     * Desktop default Opus is 80 kbit/s (see DASHCDG_AUDIO_BITRATE_KBPS). Treat ≤96 kbit/s as
     * speech/karaoke; only higher bitrates use the music signal hint.
     */
    if (bitrate_bps > 0 && bitrate_bps <= 96000) {
        opus_encoder_ctl(encoder->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    } else {
        opus_encoder_ctl(encoder->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    }
    opus_encoder_ctl(encoder->encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));
    opus_encoder_ctl(encoder->encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(encoder->encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(encoder->encoder, OPUS_SET_DTX(0));
    return 1;
}

void dashcdg_opus_encoder_free(struct dashcdg_opus_encoder *encoder) {
    if (encoder == NULL || encoder->encoder == NULL) {
        return;
    }

    opus_encoder_destroy(encoder->encoder);
    encoder->encoder = NULL;
}

int dashcdg_opus_encode_frame(
        struct dashcdg_opus_encoder *encoder,
        const int16_t *pcm,
        uint8_t *output,
        size_t output_size
) {
    if (encoder == NULL || encoder->encoder == NULL || pcm == NULL || output == NULL) {
        return -1;
    }

    return opus_encode(encoder->encoder, pcm, encoder->frame_size, output, (opus_int32) output_size);
}

int dashcdg_opus_decoder_init(
        struct dashcdg_opus_decoder *decoder,
        int sample_rate,
        int channels,
        int frame_ms
) {
    int err;

    if (decoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }

    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_size = (sample_rate * frame_ms) / 1000;
    decoder->sample_rate = sample_rate;
    decoder->channels = channels;
    decoder->decoder = opus_decoder_create(sample_rate, channels, &err);
    if (decoder->decoder == NULL || err != OPUS_OK) {
        decoder->decoder = NULL;
        return 0;
    }

    return 1;
}

void dashcdg_opus_decoder_free(struct dashcdg_opus_decoder *decoder) {
    if (decoder == NULL || decoder->decoder == NULL) {
        return;
    }

    opus_decoder_destroy(decoder->decoder);
    decoder->decoder = NULL;
}

int dashcdg_opus_decode_frame(
        struct dashcdg_opus_decoder *decoder,
        const uint8_t *input,
        size_t input_size,
        int16_t *pcm_output,
        size_t pcm_output_samples
) {
    size_t minimum_samples;

    if (decoder == NULL || decoder->decoder == NULL || input == NULL || pcm_output == NULL) {
        return -1;
    }

    minimum_samples = (size_t) decoder->frame_size * (size_t) decoder->channels;
    if (pcm_output_samples < minimum_samples) {
        return -1;
    }

    return opus_decode(
            decoder->decoder,
            input,
            (opus_int32) input_size,
            pcm_output,
            decoder->frame_size,
            0
    );
}

#endif /* DASHCDG_DESKTOP_NO_OPUS */
