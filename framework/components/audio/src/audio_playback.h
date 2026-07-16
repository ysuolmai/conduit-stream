#pragma once
#include "audio_ringbuf.h"

// FreeRTOS task entry. arg is a pointer to the audio_ringbuf_t to drain.
void audio_playback_task(void *arg);
