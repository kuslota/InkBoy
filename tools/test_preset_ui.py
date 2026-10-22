#!/usr/bin/env python3
"""Drive the real Settings and Preset menus through load, save as new, save and restore with a card directory."""
from host_build import CJSON, GB_VIDEO, PRESET_STORE_MODULES, SAVE_FILE, run

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "display_config.h"
#include "fake_game_audio.h"
#include "fake_game_storage.h"
#include "fake_gray_backend.h"
#include "fake_input.h"
#include "fake_menu.h"
#include "gray_settings_ui.h"
#include "host_fakes.h"
#include "preset_json.h"
#include "preset_store.h"
#include "settings_ui.h"

typedef struct { const char *title, *items, *subtitle; int choice, direction; void (*before)(void); } event_t;
static event_t events[128];
static unsigned event_count, event_index;

static int scripted_menu(const char *title, const char *subtitle, const char *const *items, unsigned count,
                         unsigned *selected, int *direction)
{
    (void)selected;
    assert(event_index < event_count);
    event_t e = events[event_index++];
    char joined[512] = "";
    for (unsigned i = 0; i < count; ++i) { strcat(joined, i ? "," : ""); strcat(joined, items[i]); }
    if (strcmp(title, e.title) || (e.items && strncmp(joined, e.items, strlen(e.items))) ||
        (e.subtitle && strcmp(subtitle, e.subtitle))) {
        fprintf(stderr, "event %u: got '%s' / '%s' / '%s'\n   expected '%s' / '%s' / '%s'\n", event_index, title, subtitle, joined,
                e.title, e.subtitle ? e.subtitle : "*", e.items ? e.items : "*");
        assert(0);
    }
    if (e.before) e.before();
    if (direction) *direction = e.direction ? e.direction : 1;
    return e.choice;
}

static void push(const char *title, int choice, const char *items, const char *subtitle)
{
    events[event_count++] = (event_t){title, items, subtitle, choice, 0, NULL};
}
static void push_dir(const char *title, int choice, int direction, const char *items)
{
    if (strcmp(title, "SETTINGS")) {
        events[event_count++] = (event_t){title, items, NULL, choice, direction, NULL}; return;
    }
    if (!event_count) push("SETTINGS", 0, "DISPLAY,CONTROLLER,SOUND,FPS", "");
    if (choice == 8) { push("DISPLAY", -1, NULL, NULL); push("SETTINGS", -1, NULL, NULL); return; }
    if ((choice == 3 && display_config_get().mode == DISPLAY_DITHERED) || choice == 4) {
        push("DISPLAY", 3, NULL, NULL);
        events[event_count++] = (event_t){"DITHERED TUNING", NULL, NULL, choice == 3 ? 0 : 1, direction, NULL};
        push("DITHERED TUNING", -1, NULL, NULL); return;
    }
    const char *expected = NULL;
    if (items) {
        const char *preset = strstr(items, "PRESET: ");
        if (preset) {
            const char *end = strchr(preset, ',');
            size_t length = end ? (size_t)(end - preset) : strlen(preset);
            char *label = malloc(80);
            snprintf(label, 80, "MODE: %s,BORDER: %s,%.*s", display_config_mode_name(),
                     display_config_border_name(display_config_get().border), (int)length, preset);
            expected = label;
        }
    }
    events[event_count++] = (event_t){"DISPLAY", expected, NULL, choice == 3 ? 3 : choice == 1 ? 2 : choice, direction, NULL};
}
static void push_before(const char *title, int choice, const char *items, void (*before)(void))
{
    events[event_count++] = (event_t){title, items, NULL, choice, 0, before};
}

#define SETTINGS "SETTINGS"
#define PRESET "PRESET"
#define D_ITEMS "DISPLAY: FAST DITHERED,PRESET: "
#define G_ITEMS "DISPLAY: FAST GRAYSCALE,PRESET: "

static void run_script(void)
{
    event_index = 0;
    settings_ui_open();
    if (event_index != event_count) { fprintf(stderr, "stopped after %u of %u events\n", event_index, event_count); assert(0); }
    event_count = event_index = 0;
}

static void boot(display_mode_t mode)
{
    host_fakes_reset();
    fake_gray_reset();
    fake_storage_reset();
    fake_menu_hook = scripted_menu;
    fake_menu_notices = 0;
    assert(chdir(getenv("HOST_TEST_DIR")) == 0);
    snprintf(fake_storage.root, sizeof(fake_storage.root), ".");
    nvs_handle_t h;
    assert(nvs_open("gamepad", NVS_READWRITE, &h) == ESP_OK);
    settings_ui_init(h);
    display_config_t config = {mode, 2, 2, false, BORDER_BLACK_LOGO};
    display_config_set(&config);
    assert(display_preset_load_active() == ESP_OK);
    settings_ui_load_display();
    display_config_t loaded = display_config_get();
    loaded.mode = mode;
    display_config_set(&loaded);
    gray_settings_ui_load();
}

static void put_preset(const display_preset_t *preset, const char *file)
{
    char json[PRESET_JSON_MAX], path[128];
    size_t length;
    assert(preset_json_encode(preset, json, sizeof(json), &length));
    snprintf(path, sizeof(path), "inkboy/presets/%s/%s", preset->mode == DISPLAY_DITHERED ? "dithered" : "grayscale", file);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(json, 1, length, f) == length);
    fclose(f);
}

static void reboot_without_card(void)
{
    const display_mode_t mode = display_config_get().mode;
    display_config_t config = {mode, 2, 2, false, BORDER_BLACK_LOGO};
    display_config_set(&config);
    fake_gray_reset();
    fake_storage.mount_error = ESP_FAIL;
    assert(display_preset_load_active() == ESP_OK);
    settings_ui_load_display();
    display_config_t loaded = display_config_get();
    loaded.mode = mode;
    display_config_set(&loaded);
    gray_settings_ui_load();
}

static bool exists(const char *path) { return access(path, F_OK) == 0; }

static bool read_preset(display_mode_t mode, const char *file, display_preset_t *preset)
{
    char path[128], text[PRESET_FILE_MAX + 1];
    snprintf(path, sizeof(path), "inkboy/presets/%s/%s", mode == DISPLAY_DITHERED ? "dithered" : "grayscale", file);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t length = fread(text, 1, PRESET_FILE_MAX, f);
    fclose(f);
    text[length] = 0;
    return preset_json_decode(text, length, mode, preset) == PRESET_JSON_OK;
}

static void corrupt_user_1(void)
{
    FILE *f = fopen("inkboy/presets/dithered/user-1.json", "wb");
    assert(f);
    fputs("{ not json", f);
    fclose(f);
}


static const char *const PLAIN = "LOAD PRESET,SAVE AS NEW,RESTORE DEFAULT,BACK";
static const char *const WITH_SAVE = "LOAD PRESET,SAVE AS NEW,SAVE,RESTORE DEFAULT,BACK";

static void save_new_from_default(void)
{
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 1, PLAIN, "ACTIVE: Default");
    push(PRESET, 3, PLAIN, "SAVED AS User 1");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
}

static void dithered_flow(void)
{
    boot(DISPLAY_DITHERED);
    assert(display_config_get().dither_black_pulses == 8 && display_config_get().dither_white_pulses == 8);
    push_dir(SETTINGS, 1, 1, D_ITEMS "Default,CONTROLLER: BLUETOOTH,BLACK PULSES: 8,WHITE PULSES: 8,REMAP CONTROLLER,FPS COUNTER: OFF,SOUND: ON,BACK");
    push(PRESET, 1, PLAIN, "ACTIVE: Default");
    push(PRESET, 3, PLAIN, "SAVED AS User 1");
    push_dir(SETTINGS, 3, -1, D_ITEMS "User 1,CONTROLLER: BLUETOOTH,BLACK PULSES: 8"); /* Black 8 -> 7 marks the preset modified. */
    push_dir(SETTINGS, 1, 1, D_ITEMS "User 1 *,CONTROLLER: BLUETOOTH,BLACK PULSES: 7");
    push(PRESET, 2, WITH_SAVE, "ACTIVE: User 1 *");
    push("SAVE PRESET", 0, "CANCEL,OVERWRITE", "OVERWRITE User 1?");
    push(PRESET, 2, WITH_SAVE, "ACTIVE: User 1 *");
    push("SAVE PRESET", 1, "CANCEL,OVERWRITE", "OVERWRITE User 1?");
    push(PRESET, 0, PLAIN, "SAVED User 1");
    push("LOAD PRESET", 0, "DEFAULT,User 1,BACK", "1 SAVED PRESETS ON CARD");
    push(PRESET, 3, PLAIN, "LOADED Default");
    push_dir(SETTINGS, 8, 1, D_ITEMS "Default,CONTROLLER: BLUETOOTH,BLACK PULSES: 8,WHITE PULSES: 8");
    run_script();
    display_preset_t file;
    assert(read_preset(DISPLAY_DITHERED, "user-1.json", &file) && file.black_pulses == 7 && file.white_pulses == 8 && !strcmp(file.name, "User 1"));
    assert(read_preset(DISPLAY_DITHERED, "user-1.json.bak", &file) && file.black_pulses == 8);
    assert(display_config_get().dither_black_pulses == 8 && !strcmp(display_preset_active(DISPLAY_DITHERED)->name, "Default"));
    assert(!display_preset_active_modified(DISPLAY_DITHERED) && fake_menu_notices == 0);
    assert(host_nvs_u8("inkboy", "d_modified") == 0 && !strcmp((const char *)host_nvs_find("inkboy", "d_name")->data, "Default"));

    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, NULL);
    push("LOAD PRESET", 1, "DEFAULT,User 1,BACK", NULL);
    push(PRESET, 3, PLAIN, "LOADED User 1");
    push_dir(SETTINGS, 4, 1, D_ITEMS "User 1,CONTROLLER: BLUETOOTH,BLACK PULSES: 7,WHITE PULSES: 8");
    push_dir(SETTINGS, 1, 1, D_ITEMS "User 1 *,CONTROLLER: BLUETOOTH,BLACK PULSES: 7,WHITE PULSES: 1");
    push(PRESET, 3, WITH_SAVE, "ACTIVE: User 1 *");
    push("RESTORE DEFAULT", 1, "CANCEL,RESTORE DEFAULT", "REPLACE CURRENT SETTINGS?");
    push(PRESET, 3, PLAIN, "RESTORED DEFAULT");
    push_dir(SETTINGS, 8, 1, D_ITEMS "Default,CONTROLLER: BLUETOOTH,BLACK PULSES: 8,WHITE PULSES: 8");
    run_script();
    assert(display_config_get().dither_black_pulses == 8 && display_config_get().dither_white_pulses == 8);
    assert(read_preset(DISPLAY_DITHERED, "user-1.json", &file) && file.black_pulses == 7 && file.white_pulses == 8);
    assert(!display_preset_active_modified(DISPLAY_DITHERED) && !display_preset_active_is_user(DISPLAY_DITHERED));
    assert(display_preset_default(DISPLAY_DITHERED)->black_pulses == 8 && display_preset_default(DISPLAY_DITHERED)->white_pulses == 8);

    /* Cancelling the restore confirmation changes nothing. */
    push_dir(SETTINGS, 3, -1, NULL);
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 2, PLAIN, NULL);
    push("RESTORE DEFAULT", 0, "CANCEL,RESTORE DEFAULT", NULL);
    push(PRESET, 3, PLAIN, "ACTIVE: Default *");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(display_config_get().dither_black_pulses == 7 && display_preset_active_modified(DISPLAY_DITHERED));
}

static void restart_without_card(void)
{
    boot(DISPLAY_DITHERED);
    save_new_from_default();
    push_dir(SETTINGS, 4, -1, NULL);
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(display_config_get().dither_white_pulses == 7);
    reboot_without_card();
    display_config_t config = display_config_get();
    assert(config.dither_black_pulses == 8 && config.dither_white_pulses == 7);
    assert(!strcmp(display_preset_active(DISPLAY_DITHERED)->name, "User 1") && display_preset_active_modified(DISPLAY_DITHERED));
    push_dir(SETTINGS, 8, 1, D_ITEMS "User 1 *,CONTROLLER: BLUETOOTH,BLACK PULSES: 8,WHITE PULSES: 7");
    run_script();
}

static void grayscale_flow(void)
{
    boot(DISPLAY_GRAYSCALE);
    const gray_profile_t default_gray = display_preset_default(DISPLAY_GRAYSCALE)->gray;
    assert(!memcmp(&fake_gray.config, &default_gray.tuning, sizeof(default_gray.tuning)) && fake_gray.adaptive.history_mode == 3);
    push_dir(SETTINGS, 3, 1, G_ITEMS "Default,CONTROLLER: BLUETOOTH,GRAYSCALE TUNING,GRAYSCALE TRANSITION TEST,REMAP CONTROLLER");
    push_dir("GRAYSCALE TUNING", 2, -1, NULL);
    push_dir("GRAYSCALE TUNING", 8, 1, "LIGHTEN DOSES,DARKEN DOSES,RETARGET DOSE: 75%");
    push_dir(SETTINGS, 1, 1, G_ITEMS "Default *,CONTROLLER: BLUETOOTH,GRAYSCALE TUNING");
    push(PRESET, 1, PLAIN, "ACTIVE: Default *");
    push(PRESET, 3, PLAIN, "SAVED AS User 1");
    push_dir(SETTINGS, 8, 1, G_ITEMS "User 1,CONTROLLER: BLUETOOTH,GRAYSCALE TUNING");
    run_script();
    display_preset_t file;
    assert(read_preset(DISPLAY_GRAYSCALE, "user-1.json", &file) && !strcmp(file.name, "User 1"));
    assert(file.gray.tuning.retarget_gain == 1 && display_preset_same_settings(&file, display_preset_active(DISPLAY_GRAYSCALE)));
    assert(!display_preset_active_modified(DISPLAY_GRAYSCALE) && fake_gray.config.retarget_gain == 1);

    /* Edit again, then load the saved file: the edit is dropped and the backend follows. */
    push_dir(SETTINGS, 3, 1, NULL);
    push_dir("GRAYSCALE TUNING", 2, 1, "LIGHTEN DOSES,DARKEN DOSES,RETARGET DOSE: 75%");
    push_dir("GRAYSCALE TUNING", 8, 1, NULL);
    push_dir(SETTINGS, 1, 1, G_ITEMS "User 1 *");
    push(PRESET, 0, WITH_SAVE, "ACTIVE: User 1 *");
    push("LOAD PRESET", 1, "DEFAULT,User 1,BACK", NULL);
    push(PRESET, 3, PLAIN, "LOADED User 1");
    push_dir(SETTINGS, 8, 1, G_ITEMS "User 1,CONTROLLER: BLUETOOTH,GRAYSCALE TUNING");
    run_script();
    assert(!display_preset_active_modified(DISPLAY_GRAYSCALE) && fake_gray.config.retarget_gain == 1);
    assert(display_preset_same_settings(&file, display_preset_active(DISPLAY_GRAYSCALE)));
}

static void gray_apply_failure(void)
{
    boot(DISPLAY_GRAYSCALE);
    save_new_from_default();
    display_preset_t off = *display_preset_default(DISPLAY_GRAYSCALE);
    snprintf(off.name, sizeof(off.name), "Off");
    off.gray.adaptive = (gray_adaptive_t){0, 0, {-1}, {150}};
    put_preset(&off, "off.json");
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, NULL);
    push("LOAD PRESET", 1, "DEFAULT,Off,User 1,BACK", NULL);
    push(PRESET, 3, PLAIN, "LOADED Off");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(fake_gray.adaptive.history_mode == 0 && !strcmp(display_preset_active(DISPLAY_GRAYSCALE)->name, "Off"));
    /* Default needs history memory again; when that fails nothing changes. */
    fake_gray.adaptive_alloc_error = true;
    unsigned notices = fake_menu_notices;
    const fake_gray_backend_t before = fake_gray;
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, "ACTIVE: Off");
    push("LOAD PRESET", 0, "DEFAULT,Off,User 1,BACK", NULL);
    push(PRESET, 3, PLAIN, "ACTIVE: Off");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(fake_menu_notices == notices + 1 && !strcmp(fake_menu_notice_title, "PRESET ERROR"));
    assert(!memcmp(&fake_gray.config, &before.config, sizeof(before.config)) && !memcmp(&fake_gray.adaptive, &before.adaptive, sizeof(before.adaptive)));
    assert(!strcmp(display_preset_active(DISPLAY_GRAYSCALE)->name, "Off") && fake_gray.adaptive.history_mode == 0);
    assert(!strcmp((const char *)host_nvs_find("inkboy", "g_name")->data, "Off"));
}

static void no_card(void)
{
    boot(DISPLAY_DITHERED);
    fake_storage.mount_error = ESP_FAIL;
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 1, PLAIN, "ACTIVE: Default"); /* Saving needs the card. */
    push(PRESET, 0, PLAIN, "ACTIVE: Default");
    push("LOAD PRESET", 0, "DEFAULT,BACK", "0 SAVED PRESETS ON CARD");
    push(PRESET, 2, PLAIN, "LOADED Default");
    push("RESTORE DEFAULT", 1, "CANCEL,RESTORE DEFAULT", NULL);
    push(PRESET, 3, PLAIN, "RESTORED DEFAULT");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(fake_menu_notices == 2 && !strcmp(fake_menu_notice_title, "NO SD CARD") && !exists("inkboy"));
    assert(!display_preset_active_modified(DISPLAY_DITHERED));
    /* Editing and rebooting need no card at all. */
    push_dir(SETTINGS, 3, -1, NULL);
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    reboot_without_card();
    assert(display_config_get().dither_black_pulses == 7 && display_preset_active_modified(DISPLAY_DITHERED));
}

static void invalid_file(void)
{
    boot(DISPLAY_DITHERED);
    save_new_from_default();
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, "ACTIVE: User 1");
    push_before("LOAD PRESET", 1, "DEFAULT,User 1,BACK", corrupt_user_1);
    push(PRESET, 3, PLAIN, "ACTIVE: User 1");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(fake_menu_notices == 1 && !strcmp(fake_menu_notice_title, "INVALID PRESET"));
    assert(!strcmp(display_preset_active(DISPLAY_DITHERED)->name, "User 1"));
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, NULL);
    push("LOAD PRESET", 1, "DEFAULT,BACK", "0 SAVED PRESETS ON CARD"); /* The unreadable file no longer shows up. */
    push(PRESET, 3, PLAIN, NULL);
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
}

static void write_failure(void)
{
    boot(DISPLAY_DITHERED);
    save_new_from_default();
    if (geteuid() == 0) return;
    assert(chmod("inkboy/presets/dithered", 0555) == 0);
    push_dir(SETTINGS, 3, -1, NULL);
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 2, WITH_SAVE, "ACTIVE: User 1 *");
    push("SAVE PRESET", 1, "CANCEL,OVERWRITE", NULL);
    push(PRESET, 1, WITH_SAVE, "ACTIVE: User 1 *");
    push(PRESET, 4, WITH_SAVE, "ACTIVE: User 1 *");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(chmod("inkboy/presets/dithered", 0755) == 0);
    assert(fake_menu_notices == 2 && !strcmp(fake_menu_notice_title, "SAVE FAILED"));
    assert(display_preset_active_modified(DISPLAY_DITHERED) && !exists("inkboy/presets/dithered/user-2.json"));
    display_preset_t file;
    assert(read_preset(DISPLAY_DITHERED, "user-1.json", &file) && file.black_pulses == 8);
}

static void card_full(void)
{
    boot(DISPLAY_DITHERED);
    assert(mkdir("inkboy", 0777) == 0 && mkdir("inkboy/presets", 0777) == 0 && mkdir("inkboy/presets/dithered", 0777) == 0);
    for (unsigned i = 1; i <= 18; ++i) {
        display_preset_t p = {.mode = DISPLAY_DITHERED, .black_pulses = 3, .white_pulses = 3};
        char file[32];
        snprintf(p.name, sizeof(p.name), "P%02u", i);
        snprintf(file, sizeof(file), "p%02u.json", i);
        put_preset(&p, file);
    }
    push_dir(SETTINGS, 1, 1, NULL);
    push(PRESET, 0, PLAIN, NULL);
    push("LOAD PRESET", 17, "DEFAULT,P01,P02,P03,P04,P05,P06,P07,P08,P09,P10,P11,P12,P13,P14,P15,P16,BACK", "16 SAVED PRESETS ON CARD");
    push(PRESET, 1, PLAIN, "ACTIVE: Default");
    push(PRESET, 3, PLAIN, "ACTIVE: Default");
    push_dir(SETTINGS, 8, 1, NULL);
    run_script();
    assert(fake_menu_notices == 1 && !strcmp(fake_menu_notice_title, "PRESETS FULL") && !exists("inkboy/presets/dithered/user-1.json"));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "dithered")) dithered_flow();
    else if (!strcmp(argv[1], "restart")) restart_without_card();
    else if (!strcmp(argv[1], "grayscale")) grayscale_flow();
    else if (!strcmp(argv[1], "apply_failure")) gray_apply_failure();
    else if (!strcmp(argv[1], "no_card")) no_card();
    else if (!strcmp(argv[1], "invalid")) invalid_file();
    else if (!strcmp(argv[1], "write_failure")) write_failure();
    else if (!strcmp(argv[1], "full")) card_full();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['settings_ui.c', 'controller.c', 'preset_ui.c', 'display_config.c', 'gray_settings_ui.c', 'gray_motion.c', 'screen_draw.c',
                   *PRESET_STORE_MODULES],
    fakes=['fake_menu.c', 'fake_screen.c', 'fake_input.c', 'fake_gray_backend.c', 'fake_game_audio.c', 'fake_game_storage.c',
           'fake_ble_gamepad.c', 'fake_usb_gamepad.c'],
    extra_sources=[GB_VIDEO, SAVE_FILE], vendor_sources=[CJSON],
    scenarios=[[name] for name in ('dithered', 'restart', 'grayscale', 'apply_failure', 'no_card', 'invalid', 'write_failure', 'full')])
print('PASS: Settings and Preset menus over a card directory: save as new, save with confirmation, load, restore Default, '
      'grayscale backend follow and apply failure, no card, invalid and unwritable files, full card, restart without a card')
