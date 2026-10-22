#include "display_config.h"
#include "fast_display.h"

static display_config_t current = {
    .mode = DISPLAY_GRAYSCALE,
    .dither_black_pulses = DITHER_PULSES_DEFAULT,
    .dither_white_pulses = DITHER_PULSES_DEFAULT,
    .border = BORDER_BLACK_LOGO,
};

display_config_t display_config_get(void)
{
    return current;
}

void display_config_set(const display_config_t *config)
{
    current = *config;
}

const char *display_config_mode_name(void)
{
    return current.mode == DISPLAY_DITHERED ? "FAST DITHERED" : "FAST GRAYSCALE";
}

const char *display_config_border_name(game_border_t border)
{
    static const char *const names[BORDER_COUNT] = {"BLACK LOGO", "WHITE", "WHITE LOGO", "FILL SCREEN"};
    return border < BORDER_COUNT ? names[border] : names[BORDER_WHITE];
}
