#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Sets the system time from the board's PCF85063 RTC. Call once at boot, before
 * game_audio_init: both use I2C port 0 on GPIO 47/48 and this releases it again. */
void clock_init(void);
/* Seconds that keep counting while powered off, for clock cartridges. False when no
 * trustworthy time source was found at boot. Matches gb_emulator_clock_fn. */
bool clock_now(int64_t *seconds);
