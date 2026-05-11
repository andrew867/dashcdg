#include "dashcdg/opus_codec.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include <stddef.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

/*
 * esp-opus allocates inside opus_decoder_create(); fragmentation yields OPUS_ALLOC_FAIL (-7).
 * Keep one aligned heap blob for `opus_decoder_init` (reused across init/free).
 * CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY sizes the blob for mono only (~KiB smaller vs stereo).
 */
static uint8_t *s_opus_dec_blob;
static size_t s_opus_dec_blob_cap;

static uint8_t *dashcdg_opus_ensure_dec_blob(int channels)
{
    int sz1 = opus_decoder_get_size(1);
#if !CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY
    int sz2 = opus_decoder_get_size(2);
#endif
    int need = opus_decoder_get_size(channels);
    int cap;

#if CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY
    cap = sz1;
    if (need > cap || cap <= 0) {
        return NULL;
    }
#else
    cap = sz1 > sz2 ? sz1 : sz2;

    if (need > cap) {
        cap = need;
    }
    if (cap <= 0) {
        return NULL;
    }
#endif
    if (s_opus_dec_blob != NULL && (size_t)cap <= s_opus_dec_blob_cap) {
        return s_opus_dec_blob;
    }
    if (s_opus_dec_blob != NULL) {
        heap_caps_free(s_opus_dec_blob);
        s_opus_dec_blob = NULL;
        s_opus_dec_blob_cap = 0;
    }
    {
        void *p = heap_caps_aligned_alloc(16, (size_t)cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        if (p == NULL) {
            return NULL;
        }
        s_opus_dec_blob = (uint8_t *)p;
        s_opus_dec_blob_cap = (size_t)cap;
    }
    return s_opus_dec_blob;
}

int dashcdg_opus_preallocate_decoder_blob(void)
{
    /*
     * Grab one contiguous internal-RAM block for `opus_decoder_init` before sockets / Wi-Fi
     * buffers fragment the heap (avoids OPUS_ALLOC_FAIL / harsh degraded-audio path on ESP32).
     */
    int ch = 1;
#if !CONFIG_DASHCDG_BADGE_OPUS_MONO_ONLY
    ch = 2;
#endif
    return dashcdg_opus_ensure_dec_blob(ch) != NULL ? 1 : 0;
}
#endif

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
        int frame_ms,
        int *opus_err_out
) {
    if (decoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }
    (void)opus_err_out;
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

int dashcdg_opus_decode_packet_loss(
        struct dashcdg_opus_decoder *decoder,
        int16_t *pcm_output,
        size_t pcm_output_samples
) {
    (void) pcm_output;
    (void) pcm_output_samples;
    if (decoder == NULL) {
        return -1;
    }
    return -1;
}

int dashcdg_opus_probe_packet_channels(const uint8_t *packet, size_t packet_length)
{
    (void) packet;
    (void) packet_length;
    return -1;
}

int dashcdg_opus_probe_packet_samples_per_channel(
        const uint8_t *packet,
        size_t packet_length,
        int sample_rate_hz
)
{
    (void) packet;
    (void) packet_length;
    (void) sample_rate_hz;
    return -1;
}

int dashcdg_opus_preallocate_decoder_blob(void)
{
    return 1;
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
    /*
     * Unconstrained VBR avoids bitrate-target clamping that can sound like cheap compression /
     * level pumping on programme material (especially vs narrowband chains that already band-limit).
     */
    opus_encoder_ctl(encoder->encoder, OPUS_SET_VBR_CONSTRAINT(0));
    /*
     * High complexity + in-band FEC can exceed small UDP payloads and peg CPU under load;
     * keep frames compact and encoding cheap for real-time desktop streaming.
     */
    opus_encoder_ctl(
            encoder->encoder,
            OPUS_SET_COMPLEXITY((bitrate_bps > 0 && bitrate_bps >= 96000) ? 6 : 5)
    );
    /*
     * Karaoke default: moderate bitrates favour speech (intelligibility); higher bitrates
     * favour music programme. Policy: docs/specs/opus-desktop-encoding-policy.md
     */
    /*
     * Desktop default Opus is 96 kbit/s mono (see DASHCDG_AUDIO_BITRATE_KBPS). Below that,
     * favour speech (karaoke / narrow sessions); at 96 kbit/s and above use the music programme
     * hint so brass and dense mixes are less band-limited as "voice".
     */
    if (bitrate_bps > 0 && bitrate_bps < 96000) {
        opus_encoder_ctl(encoder->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(encoder->encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));
    } else {
        opus_encoder_ctl(encoder->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        /*
         * Pin fullband when the session is 48 kHz so AUTO does not hop modes (can add "swish").
         */
        if (sample_rate >= 48000) {
            opus_encoder_ctl(encoder->encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
        } else {
            opus_encoder_ctl(encoder->encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));
        }
    }
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
        int frame_ms,
        int *opus_err_out
) {
    int err;

    if (decoder == NULL || sample_rate <= 0 || channels <= 0 || frame_ms <= 0) {
        return 0;
    }

    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_size = (sample_rate * frame_ms) / 1000;
    decoder->sample_rate = sample_rate;
    decoder->channels = channels;

#if defined(ESP_PLATFORM)
    {
        uint8_t *blob = dashcdg_opus_ensure_dec_blob(channels);

        if (blob == NULL) {
            decoder->decoder = NULL;
            if (opus_err_out != NULL) {
                *opus_err_out = OPUS_ALLOC_FAIL;
            }
            return 0;
        }
        decoder->decoder = (OpusDecoder *)blob;
        err = opus_decoder_init(decoder->decoder, sample_rate, channels);
        if (err != OPUS_OK) {
            decoder->decoder = NULL;
            if (opus_err_out != NULL) {
                *opus_err_out = err;
            }
            return 0;
        }
        /* Lower transient stack/scratch pressure during realtime decode on constrained ESP32. */
        (void)opus_decoder_ctl(decoder->decoder, OPUS_SET_COMPLEXITY(0));
        if (opus_err_out != NULL) {
            *opus_err_out = OPUS_OK;
        }
        return 1;
    }
#else
    decoder->decoder = opus_decoder_create(sample_rate, channels, &err);
    if (decoder->decoder == NULL || err != OPUS_OK) {
        decoder->decoder = NULL;
        if (opus_err_out != NULL) {
            *opus_err_out = err;
        }
        return 0;
    }

    if (opus_err_out != NULL) {
        *opus_err_out = OPUS_OK;
    }
    return 1;
#endif
}

void dashcdg_opus_decoder_free(struct dashcdg_opus_decoder *decoder) {
    if (decoder == NULL || decoder->decoder == NULL) {
        return;
    }

#if defined(ESP_PLATFORM)
    decoder->decoder = NULL;
#else
    opus_decoder_destroy(decoder->decoder);
    decoder->decoder = NULL;
#endif
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

int dashcdg_opus_decode_packet_loss(
        struct dashcdg_opus_decoder *decoder,
        int16_t *pcm_output,
        size_t pcm_output_samples
) {
    size_t minimum_samples;

    if (decoder == NULL || decoder->decoder == NULL || pcm_output == NULL) {
        return -1;
    }

    minimum_samples = (size_t) decoder->frame_size * (size_t) decoder->channels;
    if (pcm_output_samples < minimum_samples) {
        return -1;
    }

    return opus_decode(decoder->decoder, NULL, 0, pcm_output, decoder->frame_size, 0);
}

int dashcdg_opus_probe_packet_channels(const uint8_t *packet, size_t packet_length)
{
    int ch;

    if (packet == NULL || packet_length < 1U) {
        return -1;
    }
    ch = opus_packet_get_nb_channels(packet);
    if (ch < 1 || ch > 2) {
        return -1;
    }
    return ch;
}

int dashcdg_opus_probe_packet_samples_per_channel(
        const uint8_t *packet,
        size_t packet_length,
        int sample_rate_hz
)
{
    opus_int32 n;

    if (packet == NULL || packet_length < 1U || sample_rate_hz <= 0) {
        return -1;
    }
    n = opus_packet_get_nb_samples(packet, (opus_int32)packet_length, sample_rate_hz);
    return (int)n;
}

#if !defined(ESP_PLATFORM)
int dashcdg_opus_preallocate_decoder_blob(void)
{
    return 1;
}
#endif

#endif /* DASHCDG_DESKTOP_NO_OPUS */
