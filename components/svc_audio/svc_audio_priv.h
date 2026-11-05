#pragma once
// Internal hand-over of the I2S port between the playback engine and the recorder.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stops playback, waits for the engine to go idle and releases the I2S channel and the
// amplifier. Refuses (false) if a capture already holds the port or the engine does not park.
bool svc_audio_capture_acquire(void);
// Gives the port back; the next playback re-creates the TX channel and re-programs the codec.
void svc_audio_capture_release(void);

#ifdef __cplusplus
}
#endif
