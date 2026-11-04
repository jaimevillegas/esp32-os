#pragma once
// Audio engine: SD file -> MP3/WAV decoder -> PSRAM ring -> I2S -> ES8311 -> power amplifier.
//
// Everything heavy (tasks, buffers, I2S driver, codec) is created lazily at the first
// svc_audio_play(), so the engine costs only a few bytes of RAM until sound is requested.
// All functions are thread-safe and non-blocking (file I/O happens in the `audio` task).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_AUDIO_STOPPED = 0,
    SVC_AUDIO_PLAYING,
    SVC_AUDIO_PAUSED,
    SVC_AUDIO_ERROR,
} svc_audio_state_t;

typedef struct {
    svc_audio_state_t state;
    uint32_t pos_ms;
    uint32_t dur_ms;          // 0 when unknown
    uint8_t volume;           // 0..100
    char title[64];
} svc_audio_status_t;

// Called on every state change, from whichever task caused it. `track_ended` is true when the
// state became STOPPED because the file played to its end. Must not block.
typedef void (*svc_audio_event_cb_t)(const svc_audio_status_t *st, bool track_ended);

// Cheap: configures the amplifier pin (off) and stores the callback and initial volume.
void svc_audio_init(svc_audio_event_cb_t cb, uint8_t volume);

bool svc_audio_play(const char *path);     // false only when the path is unusable
void svc_audio_pause(void);
void svc_audio_resume(void);
void svc_audio_stop(void);
bool svc_audio_seek(uint32_t ms);
void svc_audio_set_volume(uint8_t percent);
void svc_audio_get_status(svc_audio_status_t *out);

#ifdef __cplusplus
}
#endif
