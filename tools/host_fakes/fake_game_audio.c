#include "game_audio.h"
#include "fake_game_audio.h"
#include "host_fakes.h"

unsigned fake_audio_enabled_calls, fake_audio_playing_on, fake_audio_playing_off, fake_audio_shutdowns;
bool fake_audio_playing, fake_audio_enabled = true;
float fake_audio_volume_db;

void game_audio_set_volume(float db) { fake_audio_volume_db = db; }

esp_err_t game_audio_init(unsigned sample_rate) { (void)sample_rate; return ESP_OK; }

void game_audio_set_enabled(bool enabled)
{
    ++fake_audio_enabled_calls;
    fake_audio_enabled = enabled;
}

void game_audio_set_playing(bool playing)
{
    fake_audio_playing = playing;
    if (playing) ++fake_audio_playing_on;
    else ++fake_audio_playing_off;
}

void game_audio_submit(const int16_t *mono, size_t sample_count) { (void)mono; (void)sample_count; }
void game_audio_shutdown(void)
{
    host_trace("game_audio_shutdown");
    ++fake_audio_shutdowns;
}
