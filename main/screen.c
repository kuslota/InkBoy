#include "screen.h"
#include "game_session.h"
#include "display_config.h"
#include "fast_display.h"
#include "screen_draw.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdatomic.h>

static const char *TAG = "player";
static DMA_ATTR uint8_t panel[SSD1681_FRAME_BYTES];
static uint8_t packed[GB_PACKED_BYTES];
static gb_video_t video;
static _Atomic bool failed;
static const uint8_t *gray_source;

uint8_t *screen_panel(void) { return panel; }
uint8_t *screen_packed_image(void) { return packed; }
gb_video_t *screen_video(void) { return &video; }
bool screen_failed(void) { return atomic_load(&failed); }
void screen_fail(void)
{
    atomic_store(&failed, true);
    game_session_wake();
}
void screen_set_gray_source(const uint8_t *image) { gray_source = image; }

esp_err_t screen_full_refresh(ssd1681_handle_t d)
{
    const display_config_t config = display_config_get();
    if (config.mode == DISPLAY_DITHERED) {
        esp_err_t error = fast_display_set_dither_pulses(config.dither_black_pulses, config.dither_white_pulses);
        if (error) return error;
    }
    return fast_display_full(d, panel, config.mode);
}

unsigned screen_partial_activations(void)
{
    return display_config_get().mode == DISPLAY_GRAYSCALE ? fast_display_gray_last_scans() : 1;
}

/* One display context spans Home, Games and Settings. Full preparation happens
 * once on entry from gameplay/status screens, not on every menu selection. */
static ssd1681_handle_t menu_display;
static bool menu_analog;
static display_mode_t menu_mode;
static uint8_t menu_dither_black, menu_dither_white;
static unsigned menu_updates, menu_settling;
static int64_t menu_last_drive;

bool screen_lifetime_reached(void)
{
    return menu_updates >= SSD1681_MAX_UPDATES - 4;
}

unsigned screen_updates(void) { return menu_updates; }
unsigned screen_pending(void) { return fast_display_pending(menu_display); }
void screen_wait_slot(int64_t image_deadline) { fast_display_wait_slot(menu_display, image_deadline); }

bool screen_close(void)
{
    esp_err_t error = ssd1681_destroy(menu_display);
    menu_display = NULL; menu_analog = false; menu_updates = menu_settling = 0;
    if (error) screen_fail();
    return error == ESP_OK;
}

bool screen_take_display(ssd1681_handle_t display, unsigned updates, bool analog)
{
    if (menu_display || !display) return false;
    const display_config_t config = display_config_get();
    menu_display = display; menu_mode = config.mode;
    menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
    /* Gameplay is continuous; start the menu's bounded refresh counter anew. */
    menu_updates = 0; menu_analog = analog;
    ESP_LOGI(TAG,"PLAYER_MENU_HANDOVER,game_updates,%u",updates);
    menu_settling = fast_display_pending(display);
    menu_last_drive = esp_timer_get_time();
    gray_source = NULL;
    return true;
}

bool screen_prompt(const char *title, const char *detail)
{
    screen_draw_message(panel, title, detail, "", "");
    return screen_present(true);
}

bool screen_present(bool changed)
{
    fast_display_set_gray_game(gray_source);
    const display_config_t config = display_config_get();
    int64_t started = esp_timer_get_time();
    esp_err_t error = ESP_OK;
    bool initial = !menu_display || screen_lifetime_reached();
    if (initial) {
        if (!screen_close()) return false;
        error = ssd1681_create(&menu_display);
        if (!error) error = ssd1681_power_on(menu_display);
    }
    bool mode_changed = menu_mode != config.mode ||
        (config.mode == DISPLAY_DITHERED && (menu_dither_black != config.dither_black_pulses || menu_dither_white != config.dither_white_pulses));
    if (!error && (initial || mode_changed)) {
        /* RAM contents and timing differ per mode, so a mode change establishes a clean reference. */
        error = screen_full_refresh(menu_display); ++menu_updates;
        menu_analog = false; menu_settling = 0; menu_mode = config.mode;
        menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
    } else if (!error) {
        error = fast_display_update(menu_display, panel); menu_updates += screen_partial_activations();
        if (!error) menu_analog = true;
        menu_settling = fast_display_pending(menu_display);
    }
    menu_last_drive = esp_timer_get_time();
    if (changed) ESP_LOGI(TAG, "PLAYER_MENU_DRAW,initial,%u,mode,%u,total_us,%" PRId64,
                         initial, menu_mode, menu_last_drive-started);
    if (error) {
        ESP_LOGE(TAG,"PLAYER_MENU_ERROR,initial,%u,mode,%u,error,%s",initial,config.mode,esp_err_to_name(error));
        screen_close(); screen_fail();
    }
    return error == ESP_OK;
}

bool screen_message(const char *title, const char *line1, const char *line2, const char *footer)
{
    fast_display_set_gray_game(NULL);
    if (!screen_close()) return false;
    ssd1681_handle_t d = NULL;
    screen_draw_message(panel, title, line1, line2, footer);
    esp_err_t error = ssd1681_create(&d);
    if (error == ESP_OK) error = ssd1681_power_on(d);
    if (error == ESP_OK) error = screen_full_refresh(d);
    esp_err_t off = ssd1681_destroy(d);
    if (error == ESP_OK) error = off;
    ESP_LOGI(TAG, "PLAYER_SCREEN,%s,result,%d", title, error);
    if (error != ESP_OK) screen_fail();
    return error == ESP_OK;
}

int64_t screen_service_deadline(void)
{
    if (menu_settling) return 0;
    return menu_analog ? menu_last_drive + 750000 : INT64_MAX;
}

int screen_service(void)
{
    if (menu_settling) {
        if (!screen_present(false)) return -1;
    } else if (menu_analog && esp_timer_get_time() >= screen_service_deadline()) {
        ssd1681_timing_t timing;
        esp_err_t error = ssd1681_analog_power(menu_display, false, &timing);
        menu_analog = false;
        if (error) { screen_close(); screen_fail(); return -1; }
    }
    return menu_settling != 0;
}

esp_err_t screen_clean(void)
{
    esp_err_t error = screen_full_refresh(menu_display);
    ++menu_updates;
    menu_analog = false;
    return error;
}
