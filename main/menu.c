#include "menu.h"
#include "controller.h"
#include "display_config.h"
#include "gb_emulator.h"
#include "input.h"
#include "diagnostics.h"
#include "menu_idle.h"
#include "screen.h"
#include "screen_draw.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "player";

static int64_t earliest(int64_t a, int64_t b) { return a < b ? a : b; }

/* Connected controllers are sampled every 10 ms as before; a waiting transport is rechecked at the
 * badge's 200 ms cadence so a new connection is noticed. Off, PC mode, parked: no polling needed. */
static int64_t controller_deadline(int64_t now, bool badge, int64_t status_check)
{
    if (input_connected()) return now + 10000;
    const controller_status_t status = controller_status();
    if ((status != CONTROLLER_BLE_WAIT && status != CONTROLLER_USB_WAIT) || controller_parked()) return INT64_MAX;
    return badge ? status_check : now + 200000;
}

static int choose(const char *title, const char *subtitle, const char *const *items,
                  unsigned count, unsigned *selected, int *direction, const char *value)
{
    const char *visible[count]; unsigned indices[count], visible_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (items[i][0] == '\033') continue;
        indices[visible_count] = i; visible[visible_count++] = items[i];
    }
    if (!visible_count) return -1;
    unsigned cursor = 0;
    for (unsigned i = 0; i < visible_count; ++i) if (indices[i] == *selected) cursor = i;
    bool redraw = true;
    bool badge = !value && (!strcmp(title,"HOME") || !strcmp(title,"CONTROLLER"));
    controller_status_t status = controller_status();
    int64_t status_check = 0;
    uint8_t held = input_buttons();
    int64_t idle_since = esp_timer_get_time();
    while (!screen_failed()) {
        *selected = indices[cursor];
        if (controller_menu_service()) status_check = 0;
        if (badge && esp_timer_get_time() >= status_check) {
            controller_status_t next = controller_status();
            if (next != status) { status=next;redraw=true; }
            status_check = esp_timer_get_time()+200000;
        }
        if (redraw) {
            if (value) screen_draw_value_menu(screen_panel(), value, visible, cursor);
            else screen_draw_menu(screen_panel(), title, subtitle, visible, visible_count, cursor);
            if (badge) screen_draw_controller_status(screen_panel(),status);
            if (!screen_present(true)) return -2;
            redraw = false; idle_since = esp_timer_get_time();
            ESP_LOGI(TAG, "PLAYER_MENU,%s,selected,%u,item,%s", title, *selected, items[*selected]);
        }
        unsigned hardware = input_hardware_event();
        if (hardware == INPUT_PC) { status=controller_status();redraw=true;continue; }
        uint8_t raw = input_buttons(), pressed = raw & (uint8_t)~held;
        held = raw;
        if (hardware || raw) idle_since = esp_timer_get_time();
        if (hardware == INPUT_BACK || (pressed & GB_BTN_B)) return -1;
        if (hardware == INPUT_SELECT || (pressed & (GB_BTN_A | GB_BTN_START | (direction ? GB_BTN_LEFT | GB_BTN_RIGHT : 0)))) {
            if (hardware == INPUT_SELECT && direction && strchr(items[*selected], ':')) {
                const char *adjust[] = {"INCREASE", "DECREASE"}; unsigned action = 0;
                int result = choose("ADJUST", "", adjust, 2, &action, NULL, items[*selected]);
                if (result == -2) return -2;
                if (result >= 0) { *direction = result == 0 ? 1 : -1; return (int)*selected; }
                redraw = true; continue;
            }
            if (direction) *direction = pressed & GB_BTN_LEFT ? -1 : 1;
            return (int)*selected;
        }
        if (hardware == INPUT_NEXT || hardware == INPUT_PREVIOUS || (pressed & (GB_BTN_UP | GB_BTN_DOWN))) {
            if (hardware == INPUT_PREVIOUS || (pressed & GB_BTN_UP)) cursor = (cursor + visible_count - 1) % visible_count;
            else cursor = (cursor + 1) % visible_count;
            redraw = true; continue;
        }
        int busy = screen_service();
        if (busy < 0) return -2;
        if (busy) continue;
        const int64_t now = esp_timer_get_time();
        const int64_t deadline = earliest(earliest(screen_service_deadline(), controller_menu_deadline()),
                                          controller_deadline(now, badge, status_check));
        if (controller_status() == CONTROLLER_USB_PC && now - idle_since < MENU_PC_AWAKE_US)
            input_wait(earliest(deadline, idle_since + MENU_PC_AWAKE_US));
        else (void)menu_idle_wait(deadline);
    }
    screen_close(); return -2;
}

int menu_choose(const char *title, const char *subtitle, const char *const *items,
                unsigned count, unsigned *selected, int *direction)
{
    return choose(title, subtitle, items, count, selected, direction, NULL);
}

void menu_notice(const char *title, const char *line1, const char *line2)
{
    if (!screen_message(title, line1, line2, "")) return;
    input_wait_release(); input_clear_events();
    while (!input_hardware_event() && !(input_buttons() & (GB_BTN_A | GB_BTN_B)))
        input_wait(esp_timer_get_time() + (input_connected() ? 20000 : 200000));
    input_wait_release();
}

bool menu_confirm(const char *title, const char *question, const char *action)
{
    const char *items[] = {"KEEP CURRENT", action}; unsigned selected = 0;
    return choose(title, "", items, 2, &selected, NULL, question) == 1;
}
