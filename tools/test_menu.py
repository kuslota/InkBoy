#!/usr/bin/env python3
"""Run the real menu engine and screen context against a scripted display and controller."""
from host_build import run

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display_config.h"
#include "fake_input.h"
#include "fake_menu_idle.h"
#include "menu_idle.h"
#include "gb_emulator.h"
#include "host_fakes.h"
#include "menu.h"
#include "screen.h"
#include "controller.h"

void screen_draw_controller_status(uint8_t *p, controller_status_t s) { (void)p;(void)s; }
static controller_status_t status_value = CONTROLLER_BLE_WAIT, flip_to;
controller_status_t controller_status(void) { return status_value; }
static bool parked_value;
static unsigned services, flip_at;
static int64_t probe_deadline = INT64_MAX;
/* Plug detection changes the status itself on the flip_at-th service call. */
bool controller_menu_service(void) { if (++services != flip_at) return false; status_value = flip_to; return true; }
int64_t controller_menu_deadline(void) { return probe_deadline; }
bool controller_parked(void) { return parked_value; }

static unsigned input_step, scenario, creates, destroys, fulls, customs, primes, offs, pending, tail = 3;
static bool analog, content_new, display_error;
static char last_title[64];
static unsigned draw_count,value_draws,menu_draws;
static int64_t update_end, analog_off_at, draw_at[2];

static unsigned back_after_waits;
static bool idle_back;
static unsigned boot_event(void)
{
    ++input_step;
    if (scenario == 10) return idle_back || fake_input_waits + fake_menu_idle_calls >= back_after_waits ? INPUT_BACK : 0;
    assert(input_step < 1000);
    if (scenario == 7) return INPUT_BACK;
    if (scenario == 8) return INPUT_SELECT;
    if (scenario == 9) return input_step == 1 ? INPUT_NEXT : INPUT_SELECT;
    if (scenario == 5) return input_step == 1 ? INPUT_NEXT : INPUT_SELECT;
    if (scenario == 6) return input_step == 2 ? INPUT_PREVIOUS : INPUT_SELECT;
    if (scenario == 0) return input_step >= 2 && input_step <= 4 ? 1 : input_step == 5 ? 2 : 0;
    if (scenario == 2) return input_step == 3 ? 2 : 0;
    if (scenario == 4) return input_step == 200 ? 2 : 0;
    return 0;
}

static uint8_t buttons(void)
{
    if (scenario == 1 && input_step) return GB_BTN_B;
    if (scenario == 4 && input_step >= 1 && input_step <= 100) return GB_BTN_UP;
    if (scenario == 2 && input_step == 2) return GB_BTN_UP;
    return 0;
}

void fast_display_set_gray_game(const uint8_t *game) { (void)game; }
unsigned fast_display_gray_last_scans(void) { return 1; }
unsigned fast_display_pending(ssd1681_handle_t d) { (void)d; return pending; }
void fast_display_wait_slot(ssd1681_handle_t d, int64_t image_deadline) { (void)d; (void)image_deadline; }
esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white) { (void)black; (void)white; return ESP_OK; }

esp_err_t fast_display_full(ssd1681_handle_t d, const uint8_t *frame, display_mode_t mode)
{
    (void)d; (void)mode;
    assert(frame == screen_panel());
    ++fulls;
    analog = false;
    pending = 0;
    host_clock_us += 1378000;
    return scenario == 3 || display_error ? 1 : ESP_OK;
}

esp_err_t fast_display_update(ssd1681_handle_t d, const uint8_t *frame)
{
    (void)d;
    assert(frame == screen_panel());
    ++customs;
    if (!analog) { analog = true; ++primes; }
    if (content_new) { pending = tail; content_new = false; }
    else if (pending) --pending;
    host_clock_us += 50000; update_end = host_clock_us;
    return scenario == 3 ? 1 : ESP_OK;
}

esp_err_t ssd1681_create(const ssd1681_config_t *config, ssd1681_handle_t *d) { (void)config; *d = (ssd1681_handle_t)1; ++creates; return ESP_OK; }
esp_err_t ssd1681_power_on(ssd1681_handle_t d) { (void)d; return ESP_OK; }
esp_err_t ssd1681_destroy(ssd1681_handle_t d) { if (d) ++destroys; analog = false; return ESP_OK; }

esp_err_t ssd1681_analog_power(ssd1681_handle_t d, bool on, ssd1681_timing_t *t)
{
    (void)d; (void)t;
    assert(!on);
    analog = false;
    ++offs; analog_off_at = host_clock_us;
    host_clock_us += 140000;
    return ESP_OK;
}

void screen_draw_value_menu(uint8_t *p, const char *value, const char *const *items, unsigned selected) { (void)p; (void)selected; ++value_draws; if (!strcmp(value,"CONFIRM TEST")) assert(!strcmp(items[0],"KEEP CURRENT") && !strcmp(items[1],"OVERWRITE")); }

void screen_draw_message(uint8_t *p, const char *a, const char *b, const char *c, const char *d) { (void)p; (void)a; (void)b; (void)c; (void)d; }

void screen_draw_menu(uint8_t *p, const char *title, const char *subtitle, const char *const *items, unsigned count, unsigned selected)
{
    (void)p; (void)subtitle; (void)items;
    assert(selected < count && strlen(title) <= 32);
    snprintf(last_title, sizeof(last_title), "%s", title);
    content_new = true; draw_count=count; draw_at[menu_draws++ % 2] = host_clock_us;
}

static void start(unsigned which)
{
    host_fakes_reset();
    scenario = which;
    input_step = 0;
    fake_input_boot_event = boot_event;
    fake_input_buttons = buttons;
}

static void set_mode(display_mode_t mode, unsigned black, unsigned white)
{
    display_config_t config = display_config_get();
    config.mode = mode;
    config.dither_black_pulses = (uint8_t)black;
    config.dither_white_pulses = (uint8_t)white;
    display_config_set(&config);
}

static const char *items[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN"};

static void navigation(void)
{
    start(0);
    unsigned selected = 0;
    int direction = 0;
    assert(menu_choose("HOME", "TEST", items, 7, &selected, &direction) == 3); /* NEXT/SELECT hardware navigation. */
    assert(fulls == 1 && customs == 3 && destroys == 0 && !screen_failed());
    assert(primes == 1 && creates == 1);
    input_step = 0; scenario = 1;
    assert(menu_choose("HOME", "TEST", items, 7, &selected, &direction) == -1); /* Controller B returns. */
    input_step = 0; scenario = 2; selected = 0;
    assert(menu_choose("GAMES", "TEST", items, 7, &selected, &direction) == 6); /* Up wraps across pages. */
    assert(fulls == 1 && destroys == 0 && creates == 1); /* Context reused across menus. */
    input_step = 0; scenario = 4; selected = 0;
    unsigned previous_customs = customs, previous_offs = offs;
    assert(menu_choose("SETTINGS", "TEST", items, 7, &selected, &direction) == 6);
    assert(customs == previous_customs + 2 + tail && offs == previous_offs + 1 && !pending); /* One held UP, settling tail, idle power off. */
    scenario = 0;
    while (screen_updates() < SSD1681_MAX_UPDATES - 4) assert(screen_present(true));
    unsigned previous_fulls = fulls;
    assert(screen_present(true) && fulls == previous_fulls + 1 && destroys == 1 && screen_updates() == 1); /* Lifetime cap reinitializes. */
    assert(screen_close() && destroys == 2);
}

static void display_failure(void)
{
    start(3);
    unsigned selected = 0;
    int direction = 0;
    assert(menu_choose("HOME", "TEST", items, 7, &selected, &direction) == -2 && screen_failed());
}

static void long_titles(void)
{
    start(1);
    set_mode(DISPLAY_DITHERED, 2, 2);
    unsigned selected = 0;
    int direction = 0;
    /* Titles that would not fit beside the mode name fall back to the title alone. */
    assert(menu_choose("HOME", "TEST", items, 7, &selected, &direction) == -1 && !strcmp(last_title, "HOME"));
    input_step = 0;
    assert(menu_choose("GRAYSCALE HISTORY / DRIVE", "TEST", items, 7, &selected, &direction) == -1 &&
           !strcmp(last_title, "GRAYSCALE HISTORY / DRIVE"));
}

static void mode_change(void)
{
    /* A mode change applies to the next menu draw with a clean reference and no gameplay scan. */
    start(0);
    set_mode(DISPLAY_DITHERED, 2, 2);
    assert(screen_present(true));
    unsigned before_fulls = fulls, before_customs = customs;
    set_mode(DISPLAY_GRAYSCALE, 2, 2);
    assert(screen_present(true) && fulls == before_fulls + 1 && customs == before_customs && !pending);
    set_mode(DISPLAY_GRAYSCALE, 5, 2);
    assert(screen_present(true) && fulls == before_fulls + 1); /* Pulse budgets only matter when dithered. */
    set_mode(DISPLAY_DITHERED, 5, 2);
    assert(screen_present(true) && fulls == before_fulls + 2);
    assert(screen_present(true) && fulls == before_fulls + 2); /* The new budget is cached: no further clean. */
    set_mode(DISPLAY_DITHERED, 5, 6);
    assert(screen_present(true) && fulls == before_fulls + 3);
    assert(screen_present(true) && fulls == before_fulls + 3);
    display_error = true;
    set_mode(DISPLAY_GRAYSCALE, 5, 6);
    before_customs = customs;
    unsigned before_destroys = destroys;
    assert(!screen_present(true) && screen_failed() && destroys == before_destroys + 1 && customs == before_customs);
}

static void hardware_navigation(void) {
    start(5); const char *rows[]={"FIRST",MENU_BACK,"SECOND",MENU_CANCEL}; unsigned selected=0;
    assert(menu_choose("MENU","",rows,4,&selected,NULL)==2 && selected==2 && draw_count==2);
    start(6);const char *value[]={"DOSE: 7"};int direction=1;selected=0;
    assert(menu_choose("TUNING","",value,1,&selected,&direction)==0 && direction==-1 && value_draws==2);
    assert(screen_close());
}

static void confirmations(void) {
    start(7);assert(!menu_confirm("CONFIRM","CONFIRM TEST","OVERWRITE"));
    start(8);assert(!menu_confirm("CONFIRM","CONFIRM TEST","OVERWRITE"));
    start(9);assert(menu_confirm("CONFIRM","CONFIRM TEST","OVERWRITE"));
    assert(screen_close());
}

static bool disconnected(void) { return false; }
static bool connected(void) { return true; }
static bool idle_now(void) { idle_back = true; return true; }

/* Each wait must target the next real deadline: analog power-down, controller recheck or attempt end. */
static int choose_once(const char *title, controller_status_t status, unsigned waits)
{
    unsigned selected = 0;
    input_step = 0; idle_back = false; status_value = status; back_after_waits = waits;
    fake_input_waits = fake_menu_idle_calls = services = menu_draws = 0;
    return menu_choose(title, "", items, 7, &selected, NULL);
}

static void waits(void)
{
    start(10);
    fake_input_wait_to_deadline = true; fake_input_connected = disconnected; fake_menu_idle_hook = idle_now;
    assert(choose_once("SETTINGS", CONTROLLER_USB_OFF, 99) == -1); /* Clean first draw: sleeps at once, nothing due. */
    assert(fake_menu_idle_calls == 1 && !fake_input_waits && fake_menu_idle_last_deadline == INT64_MAX);
    fake_menu_idle_hook = NULL;
    /* Partial draw: sleeps through settling's end to the analog power-down, which runs on time. */
    assert(choose_once("SETTINGS", CONTROLLER_USB_OFF, 2) == -1 && offs == 1 && !fake_input_waits);
    assert(analog_off_at == update_end + 750000 && fake_menu_idle_last_deadline == INT64_MAX);
    assert(choose_once("SETTINGS", CONTROLLER_BLE_WAIT, 1) == -1 && fake_menu_idle_last_deadline == fake_menu_idle_last_at + 200000);
    assert(choose_once("HOME", CONTROLLER_USB_WAIT, 1) == -1);
    assert(fake_menu_idle_last_deadline >= fake_menu_idle_last_at && fake_menu_idle_last_deadline <= fake_menu_idle_last_at + 200000);
    parked_value = true; /* Parked: the badge cannot change by itself, so no 200 ms polling. */
    unsigned before_offs = offs;
    assert(choose_once("HOME", CONTROLLER_USB_WAIT, 2) == -1 && offs == before_offs + 1 && fake_menu_idle_last_deadline == INT64_MAX);
    flip_at = 2; flip_to = CONTROLLER_USB_CONNECTED; /* A host stop/start rechecks the badge at once. */
    assert(choose_once("HOME", CONTROLLER_USB_WAIT, 1) == -1 && menu_draws == 2 && draw_at[1] - draw_at[0] < 200000);
    parked_value = false; flip_at = 0;
    probe_deadline = host_clock_us + 300000; /* A running connection attempt ends before analog power-down. */
    assert(choose_once("SETTINGS", CONTROLLER_USB_OFF, 1) == -1 && fake_menu_idle_last_deadline == probe_deadline);
    probe_deadline = INT64_MAX;
    /* USB: PC stays awake 10 s after input as before, then waits the same way. */
    assert(choose_once("SETTINGS", CONTROLLER_USB_PC, 1) == -1 && fake_input_waits == 1 && !fake_menu_idle_calls);
    assert(fake_input_last_deadline == fake_input_last_wait_at + 750000);
    assert(choose_once("SETTINGS", CONTROLLER_USB_PC, 3) == -1 && fake_input_waits == 2 && fake_menu_idle_calls == 1);
    assert(fake_input_last_deadline - fake_input_last_wait_at > MENU_PC_AWAKE_US - 1100000 && fake_menu_idle_last_deadline == INT64_MAX);
    fake_input_connected = connected;
    assert(choose_once("SETTINGS", CONTROLLER_BLE_CONNECTED, 1) == -1 && fake_menu_idle_last_deadline == fake_menu_idle_last_at + 10000);
    int64_t began = host_clock_us;
    assert(choose_once("SETTINGS", CONTROLLER_BLE_CONNECTED, 1500) == -1);
    assert(host_clock_us - began >= 14900000); /* No zero-length waits with a controller. */
    assert(screen_close());
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "confirm")) confirmations();
    else if (!strcmp(argv[1], "waits")) waits();
    else if (!strcmp(argv[1], "hardware")) hardware_navigation();
    else if (!strcmp(argv[1], "navigation")) navigation();
    else if (!strcmp(argv[1], "display_failure")) display_failure();
    else if (!strcmp(argv[1], "long_titles")) long_titles();
    else if (!strcmp(argv[1], "mode_change")) mode_change();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['menu.c', 'screen.c', 'display_config.c'], fakes=['fake_input.c', 'fake_menu_idle.c'],
    scenarios=[[name] for name in ('confirm', 'hardware', 'navigation', 'display_failure', 'long_titles', 'mode_change', 'waits')])
print('PASS: cached fast menus, press edges/held buttons, settling tail/idle power off, lifetime cap, title fallback, live mode and pulse switching, display failure, '
      'menu waits sleep at once up to the next analog/controller/attempt deadline, no badge polling while parked, PC mode 10 s awake')
