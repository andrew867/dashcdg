#ifndef DASHCDG_OPUS_CODEC_H
#define DASHCDG_OPUS_CODEC_H

#include <stddef.h>
#include <stdint.h>

#if !defined(DASHCDG_DESKTOP_NO_OPUS)
/*
 * MSYS2 / Linux: libopus headers live under include/opus/opus.h.
 * ESP-IDF managed `78/esp-opus` adds include dirs where `<opus.h>` is the public entry.
 */
#if defined(ESP_PLATFORM)
#include <opus.h>
#else
#include <opus/opus.h>
#endif
#endif

struct dashcdg_opus_encoder {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
    OpusEncoder *encoder;
#else
    void *encoder;
#endif
    int sample_rate;
    int channels;
    int frame_size;
    int bitrate_bps;
};

struct dashcdg_opus_decoder {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
    OpusDecoder *decoder;
#else
    void *decoder;
#endif
    int sample_rate;
    int channels;
    int frame_size;
};

int dashcdg_opus_encoder_init(
        struct dashcdg_opus_encoder *encoder,
        int sample_rate,
        int channels,
        int frame_ms,
        int bitrate_bps
);
void dashcdg_opus_encoder_free(struct dashcdg_opus_encoder *encoder);
int dashcdg_opus_encode_frame(
        struct dashcdg_opus_encoder *encoder,
        const int16_t *pcm,
        uint8_t *output,
        size_t output_size
);

int dashcdg_opus_decoder_init(
        struct dashcdg_opus_decoder *decoder,
        int sample_rate,
        int channels,
        int frame_ms
);
void dashcdg_opus_decoder_free(struct dashcdg_opus_decoder *decoder);
int dashcdg_opus_decode_frame(
        struct dashcdg_opus_decoder *decoder,
        const uint8_t *input,
        size_t input_size,
        int16_t *pcm_output,
        size_t pcm_output_samples
);
/** Lost packet: Opus decoder concealment (`opus_decode` with NULL payload). Returns samples per channel or <0. */
int dashcdg_opus_decode_packet_loss(
        struct dashcdg_opus_decoder *decoder,
        int16_t *pcm_output,
        size_t pcm_output_samples
);
/** Parse Opus TOC (`opus_packet_get_nb_channels`); returns 1–2 or -1 if unknown/invalid. */
int dashcdg_opus_probe_packet_channels(const uint8_t *packet, size_t packet_length);
/** Samples per channel at Fs (`opus_packet_get_nb_samples`); <0 if unknown/invalid. */
int dashcdg_opus_probe_packet_samples_per_channel(
        const uint8_t *packet,
        size_t packet_length,
        int sample_rate_hz
);

#endif
