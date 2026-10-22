#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ssd1681_fast.h"
#include "controller.h"
#include "display_config.h"

void screen_draw_fps(uint8_t *frame, unsigned tenths, game_border_t border);

void screen_draw_message(uint8_t *frame, const char *title,
                                  const char *line1, const char *line2, const char *footer);

void screen_draw_menu(uint8_t *frame, const char *title, const char *subtitle,
                              const char *const *items, unsigned count, unsigned selected);
void screen_draw_hud(uint8_t *frame, game_border_t border);

void screen_draw_gray_motion_labels(uint8_t *frame, unsigned state, bool paused);

void screen_draw_value_menu(uint8_t *frame, const char *value, const char *const *items, unsigned selected);

void screen_draw_controller_status(uint8_t *frame, controller_status_t status);
