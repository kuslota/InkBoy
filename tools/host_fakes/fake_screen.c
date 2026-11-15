#include "fake_screen.h"
#include "host_fakes.h"
#include <stdio.h>

bool fake_screen_failed, fake_screen_close_ok = true, fake_screen_message_ok = true;
unsigned fake_screen_messages;
char fake_screen_last_title[64];
static uint8_t panel[SSD1681_FRAME_BYTES], packed[GB_PACKED_BYTES];
static gb_video_t video;

uint8_t *screen_panel(void) { return panel; }
uint8_t *screen_packed_image(void) { return packed; }
gb_video_t *screen_video(void) { return &video; }
bool screen_failed(void) { return fake_screen_failed; }
void screen_fail(void) { fake_screen_failed = true; }
esp_err_t screen_full_refresh(ssd1681_handle_t d) { (void)d; return ESP_OK; }
unsigned screen_partial_activations(void) { return 1; }
bool screen_present(bool changed) { (void)changed; return true; }
int screen_service(void) { return 0; }
int64_t screen_service_deadline(void) { return INT64_MAX; }
bool screen_close(void) { return fake_screen_close_ok; }

bool screen_message(const char *title, const char *line1, const char *line2, const char *footer)
{
    (void)line1; (void)line2; (void)footer;
    host_trace("screen_message(%s)", title);
    ++fake_screen_messages;
    snprintf(fake_screen_last_title, sizeof(fake_screen_last_title), "%s", title);
    return fake_screen_message_ok;
}

void screen_set_gray_source(const uint8_t *image) { (void)image; }
bool screen_lifetime_reached(void) { return false; }
unsigned screen_updates(void) { return 0; }
unsigned screen_pending(void) { return 0; }
void screen_wait_slot(int64_t image_deadline) { (void)image_deadline; }
esp_err_t screen_clean(void) { return ESP_OK; }

bool screen_take_display(ssd1681_handle_t d, unsigned updates, bool analog) { (void)d; (void)updates; (void)analog; return true; }
bool screen_prompt(const char *title, const char *detail) { return screen_message(title, detail, "", ""); }
