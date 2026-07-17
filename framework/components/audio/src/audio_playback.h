#pragma once
#include <stdint.h>
#include "audio_ringbuf.h"

// FreeRTOS task entry. arg is a pointer to the audio_ringbuf_t to drain.
void audio_playback_task(void *arg);

// Current playback gain as Q16.16 (AUDIO_VOL_UNITY == 0 dB). Set by
// audio_set_volume() (in audio.c), read once per drain cycle so the drain applies
// software volume without the audio core exposing the storage.
int32_t audio_playback_fix_q16(void);
