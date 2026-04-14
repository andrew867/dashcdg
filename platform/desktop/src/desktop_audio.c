#define MINIMP3_IMPLEMENTATION

#include "dashcdg/desktop_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_ATOMIC_SET(value, next) __atomic_store_n(&(value), (next), __ATOMIC_RELAXED)

static struct dashcdg_pcm_buffer *dashcdg_pcm_buffer_new(uint16_t *buffer, size_t size) {
    struct dashcdg_pcm_buffer *pcm = (struct dashcdg_pcm_buffer *) malloc(sizeof(*pcm));

    if (pcm == NULL) {
        return NULL;
    }

    pcm->size = size;
    pcm->index = 0;
    pcm->buffer = buffer;
    return pcm;
}

static void dashcdg_pcm_buffer_free(struct dashcdg_pcm_buffer *pcm) {
    if (pcm == NULL) {
        return;
    }

    free(pcm->buffer);
    free(pcm);
}

#if DASHCDG_HAVE_PORTAUDIO
static void dashcdg_pcm_buffer_consume(struct dashcdg_pcm_buffer *pcm, size_t samples, uint16_t *out) {
    if (pcm == NULL || out == NULL) {
        return;
    }

    if (pcm->index + samples > pcm->size) {
        size_t available = pcm->size - pcm->index;

        if (available > 0) {
            memcpy(out, pcm->buffer + pcm->index, available * sizeof(uint16_t));
        }

        if (samples > available) {
            memset(out + available, 0, (samples - available) * sizeof(uint16_t));
        }

        pcm->index = pcm->size;
        return;
    }

    memcpy(out, pcm->buffer + pcm->index, samples * sizeof(uint16_t));
    pcm->index += samples;
}
#endif

#if DASHCDG_HAVE_PORTAUDIO
static int dashcdg_pa_callback(
        const void *input_buffer,
        void *output_buffer,
        unsigned long frame_count,
        const PaStreamCallbackTimeInfo *time_info,
        PaStreamCallbackFlags status_flags,
        void *user_data
) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) user_data;
    int seek_to_sample;
    int latency_ms;
    int audio_ts;

    (void) input_buffer;
    (void) status_flags;

    if (audio == NULL || audio->pcm == NULL) {
        return paAbort;
    }

    seek_to_sample = DASHCDG_ATOMIC_GET(audio->seek_to_sample);
    if (seek_to_sample >= 0) {
        if ((size_t) seek_to_sample > audio->pcm->size) {
            audio->pcm->index = audio->pcm->size;
        } else {
            audio->pcm->index = (size_t) seek_to_sample;
        }
        DASHCDG_ATOMIC_SET(audio->seek_to_sample, -1);
    }

    latency_ms = (int) ((time_info->outputBufferDacTime - time_info->currentTime) * 1000.0);
    audio_ts = dashcdg_desktop_audio_get_pos_ms(audio) - latency_ms;
    DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);

    dashcdg_pcm_buffer_consume(
            audio->pcm,
            frame_count * (unsigned long) audio->file_info.channels,
            (uint16_t *) output_buffer
    );

    if (audio->pcm->index >= audio->pcm->size) {
        return paComplete;
    }

    return paContinue;
}

static int dashcdg_desktop_audio_create_stream(struct dashcdg_desktop_audio *audio) {
    PaError err;

    err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    err = Pa_OpenDefaultStream(
            &audio->stream,
            0,
            audio->file_info.channels,
            paInt16,
            audio->file_info.hz,
            paFramesPerBufferUnspecified,
            dashcdg_pa_callback,
            audio
    );
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    err = Pa_StartStream(audio->stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    return 1;
}
#endif

static size_t dashcdg_ms_to_sample_index(struct dashcdg_desktop_audio *audio, uint32_t ms) {
    float samples_per_ms;
    size_t sample_index;

    if (audio == NULL || audio->file_info.channels == 0 || audio->file_info.hz == 0) {
        return 0;
    }

    samples_per_ms = (float) audio->file_info.hz / 1000.0f;
    sample_index = (size_t) ((float) ms * samples_per_ms * (float) audio->file_info.channels);
    if (sample_index > audio->file_info.samples) {
        sample_index = audio->file_info.samples;
    }

    return sample_index;
}

struct dashcdg_desktop_audio *dashcdg_desktop_audio_new(void) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) malloc(sizeof(*audio));

    if (audio == NULL) {
        return NULL;
    }

    memset(audio, 0, sizeof(*audio));
    audio->timestamp_ms = -1;
    audio->seek_to_sample = -1;
    return audio;
}

void dashcdg_desktop_audio_free(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return;
    }

#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL) {
        Pa_CloseStream(audio->stream);
        Pa_Terminate();
    }
#endif

    if (audio->pcm != NULL) {
        dashcdg_pcm_buffer_free(audio->pcm);
    }

    free(audio);
}

int dashcdg_desktop_audio_load_file(struct dashcdg_desktop_audio *audio, const char *path) {
    int err;

    if (audio == NULL || path == NULL) {
        return 0;
    }

    mp3dec_init(&audio->decoder);
    err = mp3dec_load(&audio->decoder, path, &audio->file_info, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to decode MP3: %d\n", err);
        return 0;
    }

    if (audio->pcm != NULL) {
        dashcdg_pcm_buffer_free(audio->pcm);
        audio->pcm = NULL;
    }

    audio->pcm = dashcdg_pcm_buffer_new((uint16_t *) audio->file_info.buffer, audio->file_info.samples);
    return audio->pcm != NULL;
}

int dashcdg_desktop_audio_get_pos_ms(struct dashcdg_desktop_audio *audio) {
    float samples_per_ms;

    if (audio == NULL || audio->pcm == NULL || audio->file_info.channels == 0 || audio->file_info.hz == 0) {
        return 0;
    }

    samples_per_ms = (float) audio->file_info.hz / 1000.0f;
    return (int) ((float) audio->pcm->index / samples_per_ms / (float) audio->file_info.channels);
}

void dashcdg_desktop_audio_seek_ms(struct dashcdg_desktop_audio *audio, uint32_t ms) {
    if (audio == NULL) {
        return;
    }

    DASHCDG_ATOMIC_SET(audio->seek_to_sample, (int) dashcdg_ms_to_sample_index(audio, ms));
}

int dashcdg_desktop_audio_play(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }

#if DASHCDG_HAVE_PORTAUDIO
    if (!dashcdg_desktop_audio_create_stream(audio)) {
        return 0;
    }

    while (Pa_IsStreamActive(audio->stream) == 1) {
        Pa_Sleep(50);
    }

    return 1;
#else
    uint64_t started_at_ms = dashcdg_clock_now_ms();
    uint32_t base_pos_ms = (uint32_t) dashcdg_desktop_audio_get_pos_ms(audio);
    uint32_t duration_ms = (uint32_t) dashcdg_desktop_audio_get_pos_ms(audio);

    audio->playback_running = 1;
    if (audio->file_info.hz != 0) {
        duration_ms = (uint32_t) (((uint64_t) audio->file_info.samples * 1000ULL) /
                ((uint64_t) audio->file_info.hz * (uint64_t) audio->file_info.channels));
    }

    while (audio->playback_running) {
        int seek_to_sample = DASHCDG_ATOMIC_GET(audio->seek_to_sample);
        uint64_t now_ms = dashcdg_clock_now_ms();
        uint32_t current_pos_ms;

        if (seek_to_sample >= 0) {
            audio->pcm->index = (size_t) seek_to_sample;
            base_pos_ms = (uint32_t) dashcdg_desktop_audio_get_pos_ms(audio);
            started_at_ms = now_ms;
            DASHCDG_ATOMIC_SET(audio->seek_to_sample, -1);
        }

        current_pos_ms = base_pos_ms + (uint32_t) (now_ms - started_at_ms);
        audio->pcm->index = dashcdg_ms_to_sample_index(audio, current_pos_ms);
        DASHCDG_ATOMIC_SET(audio->timestamp_ms, (int) current_pos_ms);

        if (current_pos_ms >= duration_ms || audio->pcm->index >= audio->pcm->size) {
            break;
        }

        dashcdg_sleep_ms(10);
    }

    audio->playback_running = 0;
    return 1;
#endif
}
