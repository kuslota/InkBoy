#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "display_mode.h"

/* What surrounds the game image during play. */
typedef enum { BORDER_BLACK_LOGO, BORDER_WHITE, BORDER_WHITE_LOGO, BORDER_FILL, BORDER_COUNT } game_border_t;

typedef struct {
    display_mode_t mode;
    uint8_t dither_black_pulses, dither_white_pulses;
    bool show_fps;
    game_border_t border;
} display_config_t;

display_config_t display_config_get(void);
void display_config_set(const display_config_t *config);
const char *display_config_mode_name(void);
const char *display_config_border_name(game_border_t border);
