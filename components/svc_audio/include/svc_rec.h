#pragma once
// Voice recorder: ES8311 ADC (on-board MEMS microphone) -> I2S RX -> PSRAM ring -> WAV on the SD.
//
// Nothing is allocated until svc_rec_start(): the capture and writer tasks, the PSRAM buffers
// and the I2S channel exist only while a recording runs. Recording and playback are mutually
// exclusive: starting a recording stops the audio engine, and svc_audio_play() is refused until
// the recording has ended.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_REC_RATE_HZ 16000          // 16-bit mono

typedef struct {
    bool recording;                    // a recording (or its finalisation) is in progress
    bool error;                        // the last recording failed (cleared by the next start)
    uint32_t elapsed_ms;
    uint32_t bytes;                    // size of the WAV file written so far
    uint8_t level;                     // 0..100 peak of the last ~100 ms
    char path[96];
} svc_rec_status_t;

// Called on every change of `recording` / `error`, from a recorder task. Must not block.
typedef void (*svc_rec_event_cb_t)(bool recording, bool error);

void svc_rec_init(svc_rec_event_cb_t cb);
// Creates `dir` if needed, picks rec_NNNN.wav, starts recording. Copies the path to path_out.
bool svc_rec_start(const char *dir, char *path_out, size_t cap);
void svc_rec_stop(void);               // asynchronous; the event with recording=false follows
void svc_rec_get_status(svc_rec_status_t *out);

#ifdef __cplusplus
}
#endif
