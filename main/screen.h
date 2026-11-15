#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "gb_video.h"
#include "ssd1681_fast.h"

/* Menus, gameplay and the transition test never run together, so they share
 * one set of buffers instead of spending scarce DMA-capable RAM on several. */
uint8_t *screen_panel(void);
uint8_t *screen_packed_image(void);
gb_video_t *screen_video(void);

/* Set on any unrecoverable panel or emulator error; only a reset clears it. */
bool screen_failed(void);
void screen_fail(void);

esp_err_t screen_full_refresh(ssd1681_handle_t d);
unsigned screen_partial_activations(void);

bool screen_present(bool changed);
bool screen_take_display(ssd1681_handle_t display, unsigned updates, bool analog);
bool screen_prompt(const char *title, const char *detail);
int screen_service(void);
/* When screen_service() next has work: 0 while settling, the analog power-down time, else INT64_MAX. */
int64_t screen_service_deadline(void);
bool screen_close(void);
bool screen_message(const char *title, const char *line1, const char *line2, const char *footer);

/* Borrowed, not copied: the image must stay valid until cleared with NULL. */
void screen_set_gray_source(const uint8_t *packed);
bool screen_lifetime_reached(void);
unsigned screen_updates(void);
unsigned screen_pending(void);
void screen_wait_slot(int64_t image_deadline);
esp_err_t screen_clean(void);
