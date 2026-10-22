#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes ES8311 output at sample_rate. Audio input is signed 16-bit mono. */
esp_err_t game_audio_init(unsigned sample_rate);

/* Disable output or pause playback. Both operations flush queued stale audio. */
void game_audio_set_enabled(bool enabled);
void game_audio_set_playing(bool playing);

/* Codec DAC level in dB (0 is unity). Stored now, applied when playback next starts. */
void game_audio_set_volume(float db);

/* Nonblocking bounded submission of at most 536 samples. Larger blocks are dropped;
 * queued blocks may also be dropped when the bounded queue is full. */
void game_audio_submit(const int16_t *mono, size_t sample_count);

/* Stops the worker and leaves the codec/amplifier powered down. */
void game_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
