#ifndef DASHCDG_DESKTOP_AUDIO_H
#define DASHCDG_DESKTOP_AUDIO_H

#include <pthread.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<portaudio.h>)
#include <portaudio.h>
#define DASHCDG_HAVE_PORTAUDIO 1
#else
#define DASHCDG_HAVE_PORTAUDIO 0
typedef void PaStream;
#endif
#else
#include <portaudio.h>
#define DASHCDG_HAVE_PORTAUDIO 1
#endif

#include "minimp3_ex.h"

struct dashcdg_pcm_buffer {
    size_t size;
    size_t index;
    uint16_t *buffer;
};

struct dashcdg_desktop_audio {
    struct dashcdg_pcm_buffer *pcm;
    mp3dec_t decoder;
    mp3dec_file_info_t file_info;
    PaStream *stream;
    pthread_t thread;
    int timestamp_ms;
    int seek_to_sample;
    uint64_t playback_started_ms;
    int playback_running;
};

struct dashcdg_desktop_audio *dashcdg_desktop_audio_new(void);
void dashcdg_desktop_audio_free(struct dashcdg_desktop_audio *audio);
int dashcdg_desktop_audio_load_file(struct dashcdg_desktop_audio *audio, const char *path);
int dashcdg_desktop_audio_get_pos_ms(struct dashcdg_desktop_audio *audio);
void dashcdg_desktop_audio_seek_ms(struct dashcdg_desktop_audio *audio, uint32_t ms);
int dashcdg_desktop_audio_play(struct dashcdg_desktop_audio *audio);

#endif
