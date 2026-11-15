#!/usr/bin/env python3
"""Drive the real grayscale tuning menu: load the active profile, edit, persist and restart."""
from host_build import run, GB_VIDEO

CODE = r'''
#include "gray_settings_ui.c"
#include <assert.h>
#include <stdio.h>
#include "gray_profile_store.h"
#include "fake_gray_backend.h"
#include "fake_input.h"
#include "fake_menu.h"
#include "host_fakes.h"

typedef struct { const char *title; unsigned count; int choice, direction; unsigned label_row; const char *label; } event_t;
static event_t events[64];
static unsigned event_count, event_index;

static int scripted_menu(const char *title, const char *subtitle, const char *const *items,
                         unsigned count, unsigned *selected, int *direction)
{
    (void)subtitle; (void)selected;
    assert(event_index < event_count);
    event_t e = events[event_index++];
    assert(!strcmp(title, e.title) && count == e.count);
    if (e.label) assert(!strcmp(items[e.label_row], e.label));
    if (direction) *direction = e.direction;
    return e.choice;
}

static void push(const char *title, unsigned count, int choice, int direction)
{
    events[event_count++] = (event_t){title, count, choice, direction, 0, NULL};
}

static void push_label(const char *title, unsigned count, int choice, int direction, unsigned row, const char *label)
{
    events[event_count++] = (event_t){title, count, choice, direction, row, label};
}

static void run_script(void)
{
    event_index = 0;
    gray_settings_ui_open();
    assert(event_index == event_count);
    event_count = event_index = 0;
}

#define TUNING "GRAYSCALE TUNING"
#define LIGHTEN "LIGHTEN DOSES"
#define DARKEN "DARKEN DOSES"

static void tuning(int choice, int direction) { push(TUNING, 9, choice, direction); }
static void leave(void) { push(TUNING, 9, 8, 1); }
static void lighten_dose(int direction) { push(LIGHTEN, 13, 0, direction); }

static void boot(void)
{
    host_fakes_reset();
    fake_gray_reset();
    fake_menu_hook = scripted_menu;
    gray_config = (gray_config_t)GRAY_CONFIG_DEFAULT;
    gray_adaptive = (gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
    gray_strength = 0;
    gray_immediate = false;
    gray_holds = GRAY_HOLD_ALL;
    assert(display_preset_load_active() == ESP_OK);
    gray_settings_ui_load();
}

static gray_profile_t saved_profile(void)
{
    nvs_handle_t h;
    gray_profile_t p;
    assert(nvs_open("inkboy", NVS_READONLY, &h) == ESP_OK);
    assert(gray_profile_nvs_get(h, "g_values", &p) == ESP_OK);
    nvs_close(h);
    return p;
}

static void load_applies_the_active_profile(void)
{
    boot();
    const gray_profile_t *expected = &display_preset_default(DISPLAY_GRAYSCALE)->gray;
    gray_profile_t current = gray_profile_current();
    assert(!memcmp(&current, expected, sizeof(current)));
    assert(!memcmp(&fake_gray.config, &expected->tuning, sizeof(expected->tuning)) && !memcmp(&fake_gray.adaptive, &expected->adaptive, sizeof(expected->adaptive)));
    assert(fake_gray.strength == 0 && fake_gray.immediate && fake_gray.holds == 50791);
    assert(host_nvs_commits == 0 && !display_preset_active_modified(DISPLAY_GRAYSCALE));
    /* A stored user preset wins over the Default. */
    display_preset_t user = *display_preset_default(DISPLAY_GRAYSCALE);
    snprintf(user.name, sizeof(user.name), "User 3");
    user.gray.tuning.dose[0][1] = 9;
    user.gray.strength = 3;
    assert(display_preset_set_active(&user, true) == ESP_OK);
    assert(display_preset_load_active() == ESP_OK);
    gray_settings_ui_load();
    assert(gray_config.dose[0][1] == 9 && gray_strength == 3 && fake_gray.strength == 3 && fake_gray.config.dose[0][1] == 9);
}

static void change_dose_and_save(void)
{
    boot();
    unsigned commits = host_nvs_commits;
    tuning(0, 1);
    push_label(LIGHTEN, 13, 0, 1, 0, "BLACK > DARK: 2");
    push_label(LIGHTEN, 13, 0, 1, 0, "BLACK > DARK: 3");
    push_label(LIGHTEN, 13, 12, 1, 0, "BLACK > DARK: 4");
    leave();
    run_script();
    assert(gray_config.dose[0][1] == 4 && fake_gray.config.dose[0][1] == 4);
    assert(saved_profile().tuning.dose[0][1] == 4 && host_nvs_commits == commits + 2);
    assert(display_preset_active(DISPLAY_GRAYSCALE)->gray.tuning.dose[0][1] == 4);
    assert(display_preset_active_modified(DISPLAY_GRAYSCALE) && host_nvs_u8("inkboy", "g_modified") == 1);
    assert(!strcmp(display_preset_active(DISPLAY_GRAYSCALE)->name, "Default"));
    assert(host_log_count("PLAYER_GRAY_DOSES,from,0,K,0,D,4") == 1);
    /* Doses stop at the ends of their range without writing. */
    commits = host_nvs_commits;
    tuning(0, 1);
    for (unsigned i = 0; i < 9; ++i) lighten_dose(1);
    push(LIGHTEN, 13, 12, 1);
    tuning(1, 1);
    for (unsigned i = 0; i < 3; ++i) push(DARKEN, 13, 0, -1); /* WHITE > LIGHT: 2, 1, then inert. */
    push(DARKEN, 13, 12, 1);
    leave();
    run_script();
    assert(gray_config.dose[0][1] == GRAY_DOSE_MAX && gray_config.dose[3][2] == 1);
    assert(host_nvs_commits == commits + (GRAY_DOSE_MAX - 4) + 1);
}

static void persists_across_restart(void)
{
    change_dose_and_save();
    gray_config_t expected = gray_config;
    gray_config = (gray_config_t)GRAY_CONFIG_DEFAULT;
    fake_gray_reset();
    assert(display_preset_load_active() == ESP_OK);
    gray_settings_ui_load();
    assert(!memcmp(&gray_config, &expected, sizeof(expected)) && !memcmp(&fake_gray.config, &expected, sizeof(expected)));
    assert(display_preset_active_modified(DISPLAY_GRAYSCALE));
}

static void failed_save_keeps_value(void)
{
    boot();
    unsigned notices = fake_menu_notices, applies = fake_gray.applies;
    host_nvs_commit_error = ESP_ERR_NVS_IO;
    tuning(0, 1);
    lighten_dose(1);
    push(LIGHTEN, 13, 12, 1);
    leave();
    run_script();
    host_nvs_commit_error = ESP_OK;
    assert(fake_menu_notices == notices + 1 && gray_config.dose[0][1] == 2 && fake_gray.applies == applies);
    assert(!host_nvs_find("inkboy", "g_values") && !display_preset_active_modified(DISPLAY_GRAYSCALE)); /* Nothing half-written. */
}

static void every_control_edits_the_active_preset(void)
{
    boot();
    display_preset_t user = *display_preset_default(DISPLAY_GRAYSCALE);
    snprintf(user.name, sizeof(user.name), "User 3");
    assert(display_preset_set_active(&user, false) == ESP_OK);
    /* Retarget, scans (inert in pair mode), strength and pair mode, then the dose and hold of one pair. */
    tuning(2, 1);
    push_label(TUNING, 9, 3, 1, 3, "SCANS/FRAME: PAIR");
    push_label(TUNING, 9, 5, 1, 5, "LIGHTEN: STOCK -15V");
    push_label(TUNING, 9, 6, 1, 6, "PAIR HOLD MODE: ON");
    push_label(TUNING, 9, 8, 1, 2, "RETARGET DOSE: 125%");
    run_script();
    assert(gray_config.retarget_gain == 3 && gray_strength == 1 && !gray_immediate);
    gray_profile_t p = saved_profile();
    assert(p.tuning.retarget_gain == 3 && p.strength == 1 && !p.immediate && !strcmp(display_preset_active(DISPLAY_GRAYSCALE)->name, "User 3"));
    assert(display_preset_active_modified(DISPLAY_GRAYSCALE) && fake_gray.strength == 1 && !fake_gray.immediate);
    tuning(0, 1);
    push(LIGHTEN, 13, 1, 1); /* BLACK > DARK HOLD toggles. */
    push(LIGHTEN, 13, 12, 1);
    leave();
    run_script();
    p = saved_profile();
    assert(!(gray_holds & 2u) && gray_profile_holds(&p) == gray_holds && fake_gray.holds == gray_holds);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "load")) load_applies_the_active_profile();
    else if (!strcmp(argv[1], "dose")) change_dose_and_save();
    else if (!strcmp(argv[1], "restart")) persists_across_restart();
    else if (!strcmp(argv[1], "failure")) failed_save_keeps_value();
    else if (!strcmp(argv[1], "controls")) every_control_edits_the_active_preset();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['gray_profile_store.c', 'gray_motion.c', 'screen_draw.c', 'display_config.c', 'display_preset.c'],
    fakes=['fake_menu.c', 'fake_screen.c', 'fake_input.c', 'fake_gray_backend.c'], extra_sources=[GB_VIDEO],
    scenarios=[[name] for name in ('load', 'dose', 'restart', 'failure', 'controls')])
print('PASS: grayscale tuning menu: active profile loads, edits persist into the active preset and survive restart, range limits, failed save keeps value')
