#include "screen.h"
#include "game_session.h"
#include "display_config.h"
#include "epd_present.h"
#include "fast_display.h"
#include "screen_draw.h"
#include "esp_attr.h"
#include <stdatomic.h>

static DMA_ATTR uint8_t panel[SSD1681_FRAME_BYTES];
static uint8_t packed[GB_PACKED_BYTES];
static gb_video_t video;
static _Atomic bool failed;

uint8_t *screen_panel(void) { return panel; }
uint8_t *screen_packed_image(void) { return packed; }
gb_video_t *screen_video(void) { return &video; }
bool screen_failed(void) { return atomic_load(&failed); }
void screen_fail(void)
{
    atomic_store(&failed, true);
    game_session_wake();
}

static epd_present_mode_t present_mode(void)
{
    const display_config_t config = display_config_get();
    return (epd_present_mode_t){.mode = config.mode, .dither_black = config.dither_black_pulses,
                                .dither_white = config.dither_white_pulses};
}

/* One display context spans Home, Games and Settings. Full preparation happens
 * once on entry from gameplay/status screens, not on every menu selection. */
static epd_present_t presenter = {.config = {.frame = panel, .mode = present_mode, .on_fail = screen_fail,
                                             .log_tag = "player"}};

void screen_set_gray_source(const uint8_t *image) { epd_present_set_gray_source(&presenter, image); }
esp_err_t screen_full_refresh(ssd1681_handle_t d) { return epd_present_full_refresh(&presenter, d); }
unsigned screen_partial_activations(void) { return epd_present_partial_activations(&presenter); }
bool screen_lifetime_reached(void) { return epd_present_lifetime_reached(&presenter); }
unsigned screen_updates(void) { return epd_present_updates(&presenter); }
unsigned screen_pending(void) { return epd_present_pending(&presenter); }
void screen_wait_slot(int64_t image_deadline) { epd_present_wait_slot(&presenter, image_deadline); }
bool screen_close(void) { return epd_present_close(&presenter); }
bool screen_take_display(ssd1681_handle_t display, unsigned updates, bool analog)
{
    return epd_present_take(&presenter, display, updates, analog);
}
bool screen_present(bool changed) { return epd_present(&presenter, changed); }
int64_t screen_service_deadline(void) { return epd_present_service_deadline(&presenter); }
int screen_service(void) { return epd_present_service(&presenter); }
esp_err_t screen_clean(void) { return epd_present_clean(&presenter); }

bool screen_prompt(const char *title, const char *detail)
{
    screen_draw_message(panel, title, detail, "", "");
    return screen_present(true);
}

bool screen_message(const char *title, const char *line1, const char *line2, const char *footer)
{
    fast_display_set_gray_game(NULL);
    if (!screen_close()) return false;
    screen_draw_message(panel, title, line1, line2, footer);
    return epd_present_once(&presenter, title);
}
